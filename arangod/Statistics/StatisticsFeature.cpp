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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "StatisticsFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Basics/NumberOfCores.h"
#include "Basics/PhysicalMemory.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Basics/process-utils.h"
#include "Basics/system-functions.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "FeaturePhases/ServerFeaturePhase.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Metrics/Builder.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/FixScale.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/Metric.h"
#include "Metrics/MetricsFeature.h"
#include "Network/NetworkFeature.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/CpuUsageFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Statistics/ConnectionStatistics.h"
#include "Statistics/Descriptions.h"
#include "Statistics/RequestStatistics.h"
#include "Statistics/ServerStatistics.h"
#include "Statistics/StatisticsWorker.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/ExecContext.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/vocbase.h"

#include <initializer_list>
#include <chrono>
#include <thread>

#include <absl/strings/str_cat.h>
#include <velocypack/Builder.h>

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;
using namespace arangodb::statistics;

// -----------------------------------------------------------------------------
// --SECTION--                                                  global variables
// -----------------------------------------------------------------------------

namespace {
std::string const stats15Query =
    "/*stats15*/ FOR s IN @@collection FILTER s.time > @start FILTER "
    "s.clusterId IN @clusterIds SORT s.time COLLECT clusterId = s.clusterId "
    "INTO clientConnections = s.client.httpConnections LET "
    "clientConnectionsCurrent = LAST(clientConnections) COLLECT AGGREGATE "
    "clientConnections15M = SUM(clientConnectionsCurrent) RETURN "
    "{clientConnections15M: clientConnections15M || 0}";

std::string const statsSamplesQuery =
    "/*statsSample*/ FOR s IN @@collection FILTER s.time > @start FILTER "
    "s.clusterId IN @clusterIds RETURN { time: s.time, clusterId: s.clusterId, "
    "physicalMemory: s.server.physicalMemory, residentSizeCurrent: "
    "s.system.residentSize, clientConnectionsCurrent: "
    "s.client.httpConnections, avgRequestTime: s.client.avgRequestTime, "
    "bytesSentPerSecond: s.client.bytesSentPerSecond, bytesReceivedPerSecond: "
    "s.client.bytesReceivedPerSecond, http: { optionsPerSecond: "
    "s.http.requestsOptionsPerSecond, putsPerSecond: "
    "s.http.requestsPutPerSecond, headsPerSecond: "
    "s.http.requestsHeadPerSecond, postsPerSecond: "
    "s.http.requestsPostPerSecond, getsPerSecond: s.http.requestsGetPerSecond, "
    "deletesPerSecond: s.http.requestsDeletePerSecond, othersPerSecond: "
    "s.http.requestsOptionsPerSecond, patchesPerSecond: "
    "s.http.requestsPatchPerSecond } }";

}  // namespace

namespace arangodb {
namespace statistics {

std::initializer_list<double> const BytesReceivedDistributionCuts{
    250, 1000, 2000, 5000, 10000};
std::initializer_list<double> const BytesSentDistributionCuts{250, 1000, 2000,
                                                              5000, 10000};
std::initializer_list<double> const ConnectionTimeDistributionCuts{0.1, 1.0,
                                                                   60.0};
std::initializer_list<double> const RequestTimeDistributionCuts{
    0.01, 0.05, 0.1, 0.2, 0.5, 1.0, 5.0, 15.0, 30.0};

struct BytesReceivedScale {
  static metrics::FixScale<double> scale() {
    return {250, 10000, BytesReceivedDistributionCuts};
  }
};

struct BytesSentScale {
  static metrics::FixScale<double> scale() {
    return {250, 10000, BytesSentDistributionCuts};
  }
};

struct ConnectionTimeScale {
  static metrics::FixScale<double> scale() {
    return {0.1, 60.0, ConnectionTimeDistributionCuts};
  }
};

struct RequestTimeScale {
  static metrics::FixScale<double> scale() {
    return {0.01, 30.0, RequestTimeDistributionCuts};
  }
};

DECLARE_HISTOGRAM(arangodb_client_connection_statistics_bytes_received,
                  BytesReceivedScale, "Bytes received for requests");
DECLARE_HISTOGRAM(arangodb_client_connection_statistics_bytes_sent,
                  BytesSentScale, "Bytes sent for responses");
DECLARE_HISTOGRAM(arangodb_client_user_connection_statistics_bytes_received,
                  BytesReceivedScale,
                  "Bytes received for requests, only user traffic");
DECLARE_HISTOGRAM(arangodb_client_user_connection_statistics_bytes_sent,
                  BytesSentScale,
                  "Bytes sent for responses, only user traffic");
DECLARE_COUNTER(
    arangodb_process_statistics_minor_page_faults_total,
    "The number of minor faults the process has made which have not required "
    "loading a memory page from disk");
DECLARE_COUNTER(arangodb_process_statistics_major_page_faults_total,
                "This figure contains the number of major faults the process "
                "has made which have required loading a memory page from disk");
DECLARE_GAUGE(arangodb_process_statistics_user_time, double,
              "Amount of time that this process has been scheduled in user "
              "mode, measured in seconds");
DECLARE_GAUGE(arangodb_process_statistics_system_time, double,
              "Amount of time that this process has been scheduled in kernel "
              "mode, measured in seconds");
DECLARE_GAUGE(arangodb_process_statistics_number_of_threads, double,
              "Number of threads in the arangod process");
DECLARE_GAUGE(
    arangodb_process_statistics_resident_set_size, double,
    "The total size of the number of pages the process has in real memory. "
    "This is just the pages which count toward text, data, or stack space. "
    "This does not include pages which have not been demand-loaded in, or "
    "which are swapped out. The resident set size is reported in bytes");
DECLARE_GAUGE(arangodb_process_statistics_resident_set_size_percent, double,
              "The relative size of the number of pages the process has in "
              "real memory compared to system memory. This is just the pages "
              "which count toward text, data, or stack space. This does not "
              "include pages which have not been demand-loaded in, or which "
              "are swapped out. The value is a ratio between 0.00 and 1.00");
DECLARE_GAUGE(
    arangodb_process_statistics_virtual_memory_size, double,
    "This figure contains The size of the virtual memory the process is using");
DECLARE_GAUGE(arangodb_client_connection_statistics_client_connections, double,
              "The number of client connections that are currently open");
DECLARE_HISTOGRAM(arangodb_client_connection_statistics_connection_time,
                  ConnectionTimeScale, "Total connection time of a client");
DECLARE_HISTOGRAM(arangodb_client_connection_statistics_total_time,
                  ConnectionTimeScale, "Total time needed to answer a request");
DECLARE_HISTOGRAM(arangodb_client_connection_statistics_request_time,
                  RequestTimeScale, "Request time needed to answer a request");
DECLARE_HISTOGRAM(arangodb_client_connection_statistics_queue_time,
                  RequestTimeScale, "Queue time needed to answer a request");
DECLARE_HISTOGRAM(arangodb_client_connection_statistics_io_time,
                  RequestTimeScale, "IO time needed to answer a request");
DECLARE_COUNTER(arangodb_http_request_statistics_total_requests_total,
                "Total number of HTTP requests");
DECLARE_COUNTER(arangodb_http_request_statistics_superuser_requests_total,
                "Total number of HTTP requests executed by superuser/JWT");
DECLARE_COUNTER(arangodb_http_request_statistics_user_requests_total,
                "Total number of HTTP requests executed by clients");
DECLARE_COUNTER(arangodb_http_request_statistics_async_requests_total,
                "Number of asynchronously executed HTTP requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_delete_requests_total,
                "Number of HTTP DELETE requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_get_requests_total,
                "Number of HTTP GET requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_head_requests_total,
                "Number of HTTP HEAD requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_options_requests_total,
                "Number of HTTP OPTIONS requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_patch_requests_total,
                "Number of HTTP PATCH requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_post_requests_total,
                "Number of HTTP POST requests");
DECLARE_COUNTER(arangodb_http_request_statistics_http_put_requests_total,
                "Number of HTTP PUT requests");
DECLARE_COUNTER(arangodb_http_request_statistics_other_http_requests_total,
                "Number of other HTTP requests");
DECLARE_COUNTER(arangodb_server_statistics_server_uptime_total,
                "Number of seconds elapsed since server start");
DECLARE_GAUGE(arangodb_server_statistics_physical_memory, double,
              "Physical memory in bytes");
DECLARE_GAUGE(arangodb_server_statistics_cpu_cores, double,
              "Number of CPU cores visible to the arangod process");
DECLARE_GAUGE(
    arangodb_server_statistics_user_percent, double,
    "Percentage of time that the system CPUs have spent in user mode");
DECLARE_GAUGE(
    arangodb_server_statistics_system_percent, double,
    "Percentage of time that the system CPUs have spent in kernel mode");
DECLARE_GAUGE(arangodb_server_statistics_idle_percent, double,
              "Percentage of time that the system CPUs have been idle");
DECLARE_GAUGE(
    arangodb_server_statistics_iowait_percent, double,
    "Percentage of time that the system CPUs have been waiting for I/O");
DECLARE_GAUGE(arangodb_v8_context_alive, double,
              "Number of V8 contexts currently alive");
DECLARE_GAUGE(arangodb_v8_context_busy, double,
              "Number of V8 contexts currently busy");
DECLARE_GAUGE(arangodb_v8_context_dirty, double,
              "Number of V8 contexts currently dirty");
DECLARE_GAUGE(arangodb_v8_context_free, double,
              "Number of V8 contexts currently free");
DECLARE_GAUGE(arangodb_v8_context_max, double,
              "Maximum number of concurrent V8 contexts");
DECLARE_GAUGE(arangodb_v8_context_min, double,
              "Minimum number of concurrent V8 contexts");
DECLARE_GAUGE(arangodb_request_statistics_memory_usage, uint64_t,
              "Memory used by the internal request statistics");
DECLARE_GAUGE(arangodb_connection_statistics_memory_usage, uint64_t,
              "Memory used by the internal connection statistics");

namespace {
// local_name: {"prometheus_name", "type", "help"}
auto const statStrings = std::map<std::string_view,
                                  std::vector<std::string_view>>{
    {"bytesReceived",
     {"arangodb_client_connection_statistics_bytes_received", "histogram",
      "Bytes received for requests"}},
    {"bytesSent",
     {"arangodb_client_connection_statistics_bytes_sent", "histogram",
      "Bytes sent for responses"}},
    {"bytesReceivedUser",
     {"arangodb_client_user_connection_statistics_bytes_received", "histogram",
      "Bytes received for requests, only user traffic"}},
    {"bytesSentUser",
     {"arangodb_client_user_connection_statistics_bytes_sent", "histogram",
      "Bytes sent for responses, only user traffic"}},
    {"minorPageFaults",
     {"arangodb_process_statistics_minor_page_faults_total", "counter",
      "The number of minor faults the process has made which have not required "
      "loading a memory page from disk"}},
    {"majorPageFaults",
     {"arangodb_process_statistics_major_page_faults_total", "counter",
      "This figure contains the number of major faults the "
      "process has made which have required loading a memory page from disk"}},
    {"userTime",
     {"arangodb_process_statistics_user_time", "gauge",
      "Amount of time that this process has been scheduled in user mode, "
      "measured in seconds"}},
    {"systemTime",
     {"arangodb_process_statistics_system_time", "gauge",
      "Amount of time that this process has been scheduled in kernel mode, "
      "measured in seconds"}},
    {"numberOfThreads",
     {"arangodb_process_statistics_number_of_threads", "gauge",
      "Number of threads in the arangod process"}},
    {"residentSize",
     {"arangodb_process_statistics_resident_set_size", "gauge",
      "The total size of the number of pages the process has in real memory. "
      "This is just the pages which count toward text, data, or stack space. "
      "This does not include pages which have not been demand-loaded in, or "
      "which are swapped out. The resident set size is reported in bytes"}},
    {"residentSizePercent",
     {"arangodb_process_statistics_resident_set_size_percent", "gauge",
      "The relative size of the number of pages the process has in real memory "
      "compared to system memory. This is just the pages which count toward "
      "text, data, or stack space. This does not include pages which have not "
      "been demand-loaded in, or which are swapped out. The value is a ratio "
      "between 0.00 and 1.00"}},
    {"virtualSize",
     {"arangodb_process_statistics_virtual_memory_size", "gauge",
      "This figure contains The size of the virtual memory the process is "
      "using"}},
    {"clientHttpConnections",
     {"arangodb_client_connection_statistics_client_connections", "gauge",
      "The number of client connections that are currently open"}},
    {"connectionTime",
     {"arangodb_client_connection_statistics_connection_time", "histogram",
      "Total connection time of a client"}},
    {"connectionTimeCount",
     {"arangodb_client_connection_statistics_connection_time_count", "gauge",
      "Total connection time of a client"}},
    {"connectionTimeSum",
     {"arangodb_client_connection_statistics_connection_time_sum", "gauge",
      "Total connection time of a client"}},
    {"totalTime",
     {"arangodb_client_connection_statistics_total_time", "histogram",
      "Total time needed to answer a request"}},
    {"totalTimeCount",
     {"arangodb_client_connection_statistics_total_time_count", "gauge",
      "Total time needed to answer a request"}},
    {"totalTimeSum",
     {"arangodb_client_connection_statistics_total_time_sum", "gauge",
      "Total time needed to answer a request"}},
    {"requestTime",
     {"arangodb_client_connection_statistics_request_time", "histogram",
      "Request time needed to answer a request"}},
    {"requestTimeCount",
     {"arangodb_client_connection_statistics_request_time_count", "gauge",
      "Request time needed to answer a request"}},
    {"requestTimeSum",
     {"arangodb_client_connection_statistics_request_time_sum", "gauge",
      "Request time needed to answer a request"}},
    {"queueTime",
     {"arangodb_client_connection_statistics_queue_time", "histogram",
      "Request time needed to answer a request"}},
    {"queueTimeCount",
     {"arangodb_client_connection_statistics_queue_time_count", "gauge",
      "Request time needed to answer a request"}},
    {"queueTimeSum",
     {"arangodb_client_connection_statistics_queue_time_sum", "gauge",
      "Request time needed to answer a request"}},
    {"ioTime",
     {"arangodb_client_connection_statistics_io_time", "histogram",
      "Request time needed to answer a request"}},
    {"ioTimeCount",
     {"arangodb_client_connection_statistics_io_time_count", "gauge",
      "Queue time needed to answer a request"}},
    {"ioTimeSum",
     {"arangodb_client_connection_statistics_io_time_sum", "gauge",
      "IO time needed to answer a request"}},
    {"httpReqsTotal",
     {"arangodb_http_request_statistics_total_requests_total", "counter",
      "Total number of HTTP requests"}},
    {"httpReqsSuperuser",
     {"arangodb_http_request_statistics_superuser_requests_total", "counter",
      "Total number of HTTP requests executed by superuser/JWT"}},
    {"httpReqsUser",
     {"arangodb_http_request_statistics_user_requests_total", "counter",
      "Total number of HTTP requests executed by clients"}},
    {"httpReqsAsync",
     {"arangodb_http_request_statistics_async_requests_total", "counter",
      "Number of asynchronously executed HTTP requests"}},
    {"httpReqsDelete",
     {"arangodb_http_request_statistics_http_delete_requests_total", "counter",
      "Number of HTTP DELETE requests"}},
    {"httpReqsGet",
     {"arangodb_http_request_statistics_http_get_requests_total", "counter",
      "Number of HTTP GET requests"}},
    {"httpReqsHead",
     {"arangodb_http_request_statistics_http_head_requests_total", "counter",
      "Number of HTTP HEAD requests"}},
    {"httpReqsOptions",
     {"arangodb_http_request_statistics_http_options_requests_total", "counter",
      "Number of HTTP OPTIONS requests"}},
    {"httpReqsPatch",
     {"arangodb_http_request_statistics_http_patch_requests_total", "counter",
      "Number of HTTP PATCH requests"}},
    {"httpReqsPost",
     {"arangodb_http_request_statistics_http_post_requests_total", "counter",
      "Number of HTTP POST requests"}},
    {"httpReqsPut",
     {"arangodb_http_request_statistics_http_put_requests_total", "counter",
      "Number of HTTP PUT requests"}},
    {"httpReqsOther",
     {"arangodb_http_request_statistics_other_http_requests_total", "counter",
      "Number of other HTTP requests"}},
    {"uptime",
     {"arangodb_server_statistics_server_uptime_total", "counter",
      "Number of seconds elapsed since server start"}},
    {"physicalSize",
     {"arangodb_server_statistics_physical_memory", "gauge",
      "Physical memory in bytes"}},
    {"cores",
     {"arangodb_server_statistics_cpu_cores", "gauge",
      "Number of CPU cores visible to the arangod process"}},
    {"userPercent",
     {"arangodb_server_statistics_user_percent", "gauge",
      "Percentage of time that the system CPUs have spent in user mode"}},
    {"systemPercent",
     {"arangodb_server_statistics_system_percent", "gauge",
      "Percentage of time that the system CPUs have spent in kernel mode"}},
    {"idlePercent",
     {"arangodb_server_statistics_idle_percent", "gauge",
      "Percentage of time that the system CPUs have been idle"}},
    {"iowaitPercent",
     {"arangodb_server_statistics_iowait_percent", "gauge",
      "Percentage of time that the system CPUs have been waiting for I/O"}},
    {"v8ContextAvailable",
     {"arangodb_v8_context_alive", "gauge",
      "Number of V8 contexts currently alive"}},
    {"v8ContextBusy",
     {"arangodb_v8_context_busy", "gauge",
      "Number of V8 contexts currently busy"}},
    {"v8ContextDirty",
     {"arangodb_v8_context_dirty", "gauge",
      "Number of V8 contexts currently dirty"}},
    {"v8ContextFree",
     {"arangodb_v8_context_free", "gauge",
      "Number of V8 contexts currently free"}},
    {"v8ContextMax",
     {"arangodb_v8_context_max", "gauge",
      "Maximum number of concurrent V8 contexts"}},
    {"v8ContextMin",
     {"arangodb_v8_context_min", "gauge",
      "Minimum number of concurrent V8 contexts"}},
};

// Connect legacy statistics with metrics definitions for automatic checks
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
using StatBuilder =
    std::unordered_map<std::string_view,
                       std::unique_ptr<metrics::Builder const> const>;
auto makeStatBuilder(std::initializer_list<
                     std::pair<std::string_view, metrics::Builder const* const>>
                         initList) -> StatBuilder {
  auto unomap = StatBuilder{};
  unomap.reserve(initList.size());
  for (auto const& it : initList) {
    unomap.emplace(it.first, it.second);
  }
  return unomap;
}
auto const statBuilder = makeStatBuilder({
    {"bytesReceived",
     new arangodb_client_connection_statistics_bytes_received()},
    {"bytesSent", new arangodb_client_connection_statistics_bytes_sent()},
    {"bytesReceivedUser",
     new arangodb_client_user_connection_statistics_bytes_received()},
    {"bytesSentUser",
     new arangodb_client_user_connection_statistics_bytes_sent()},
    {"minorPageFaults",
     new arangodb_process_statistics_minor_page_faults_total()},
    {"majorPageFaults",
     new arangodb_process_statistics_major_page_faults_total()},
    {"userTime", new arangodb_process_statistics_user_time()},
    {"systemTime", new arangodb_process_statistics_system_time()},
    {"numberOfThreads", new arangodb_process_statistics_number_of_threads()},
    {"residentSize", new arangodb_process_statistics_resident_set_size()},
    {"residentSizePercent",
     new arangodb_process_statistics_resident_set_size_percent()},
    {"virtualSize", new arangodb_process_statistics_virtual_memory_size()},
    {"clientHttpConnections",
     new arangodb_client_connection_statistics_client_connections()},
    {"connectionTime",
     new arangodb_client_connection_statistics_connection_time()},
    {"connectionTimeCount", nullptr},
    {"connectionTimeSum", nullptr},
    {"totalTime", new arangodb_client_connection_statistics_total_time()},
    {"totalTimeCount", nullptr},
    {"totalTimeSum", nullptr},
    {"requestTime", new arangodb_client_connection_statistics_request_time()},
    {"requestTimeCount", nullptr},
    {"requestTimeSum", nullptr},
    {"queueTime", new arangodb_client_connection_statistics_queue_time()},
    {"queueTimeCount", nullptr},
    {"queueTimeSum", nullptr},
    {"ioTime", new arangodb_client_connection_statistics_io_time()},
    {"ioTimeCount", nullptr},
    {"ioTimeSum", nullptr},
    {"httpReqsTotal",
     new arangodb_http_request_statistics_total_requests_total()},
    {"httpReqsSuperuser",
     new arangodb_http_request_statistics_superuser_requests_total()},
    {"httpReqsUser",
     new arangodb_http_request_statistics_user_requests_total()},
    {"httpReqsAsync",
     new arangodb_http_request_statistics_async_requests_total()},
    {"httpReqsDelete",
     new arangodb_http_request_statistics_http_delete_requests_total()},
    {"httpReqsGet",
     new arangodb_http_request_statistics_http_get_requests_total()},
    {"httpReqsHead",
     new arangodb_http_request_statistics_http_head_requests_total()},
    {"httpReqsOptions",
     new arangodb_http_request_statistics_http_options_requests_total()},
    {"httpReqsPatch",
     new arangodb_http_request_statistics_http_patch_requests_total()},
    {"httpReqsPost",
     new arangodb_http_request_statistics_http_post_requests_total()},
    {"httpReqsPut",
     new arangodb_http_request_statistics_http_put_requests_total()},
    {"httpReqsOther",
     new arangodb_http_request_statistics_other_http_requests_total()},
    {"uptime", new arangodb_server_statistics_server_uptime_total()},
    {"physicalSize", new arangodb_server_statistics_physical_memory()},
    {"cores", new arangodb_server_statistics_cpu_cores()},
    {"userPercent", new arangodb_server_statistics_user_percent()},
    {"systemPercent", new arangodb_server_statistics_system_percent()},
    {"idlePercent", new arangodb_server_statistics_idle_percent()},
    {"iowaitPercent", new arangodb_server_statistics_iowait_percent()},
    {"v8ContextAvailable", new arangodb_v8_context_alive()},
    {"v8ContextBusy", new arangodb_v8_context_busy()},
    {"v8ContextDirty", new arangodb_v8_context_dirty()},
    {"v8ContextFree", new arangodb_v8_context_free()},
    {"v8ContextMax", new arangodb_v8_context_max()},
    {"v8ContextMin", new arangodb_v8_context_min()},
});
#endif

}  // namespace

Counter AsyncRequests;
Counter HttpConnections;
Counter TotalRequests;
Counter TotalRequestsSuperuser;
Counter TotalRequestsUser;
MethodRequestCounters MethodRequests;
Distribution ConnectionTimeDistribution(ConnectionTimeDistributionCuts);

RequestFigures::RequestFigures()
    : bytesReceivedDistribution(BytesReceivedDistributionCuts),
      bytesSentDistribution(BytesSentDistributionCuts),
      ioTimeDistribution(RequestTimeDistributionCuts),
      queueTimeDistribution(RequestTimeDistributionCuts),
      requestTimeDistribution(RequestTimeDistributionCuts),
      totalTimeDistribution(RequestTimeDistributionCuts) {}

RequestFigures SuperuserRequestFigures;
RequestFigures UserRequestFigures;
}  // namespace statistics
}  // namespace arangodb

// -----------------------------------------------------------------------------
// --SECTION--                                                  StatisticsThread
// -----------------------------------------------------------------------------

class StatisticsThread final : public ServerThread<ArangodServer> {
 public:
  explicit StatisticsThread(Server& server)
      : ServerThread<ArangodServer>(server, "Statistics") {}
  ~StatisticsThread() { shutdown(); }

 public:
  void run() override {
    constexpr uint64_t const kMinIdleSleepTime = 100;
    constexpr uint64_t const kMaxIdleSleepTime = 250;

    uint64_t sleepTime = kMinIdleSleepTime;
    int nothingHappened = 0;

    while (!isStopping()) {
      size_t count = 0;
      try {
        count = RequestStatistics::processAll();
      } catch (std::exception const& ex) {
        LOG_TOPIC("82524", WARN, Logger::STATISTICS)
            << "caught exception during request statistics processing: "
            << ex.what();
      }

      if (count == 0) {
        // nothing needed to be processed
        if (++nothingHappened == 10 * 30) {
          // increase sleep time every 30 seconds
          nothingHappened = 0;
          sleepTime = std::min(sleepTime + 50, kMaxIdleSleepTime);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));

      } else {
        // something needed to be processed
        nothingHappened = 0;
        sleepTime = kMinIdleSleepTime;

        if (count < 10) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else if (count < 100) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    }
  }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                 StatisticsFeature
// -----------------------------------------------------------------------------

StatisticsFeature::StatisticsFeature(Server& server)
    : ArangodFeature{server, *this},
      _statistics(true),
      _statisticsHistory(true),
      _statisticsHistoryTouched(false),
      _statisticsAllDatabases(true),
      _descriptions(server),
      _requestStatisticsMemoryUsage{
          server.getFeature<metrics::MetricsFeature>().add(
              arangodb_request_statistics_memory_usage{})},
      _connectionStatisticsMemoryUsage{
          server.getFeature<metrics::MetricsFeature>().add(
              arangodb_connection_statistics_memory_usage{})} {
  setOptional(true);
  startsAfter<AqlFeaturePhase>();
  startsAfter<NetworkFeature>();

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  bool foundError = false;
  for (auto const& it : statBuilder) {
    if (auto const& statIt = statStrings.find(it.first);
        statIt != statStrings.end()) {
      if (it.second != nullptr) {
        auto const& builder = *it.second;
        auto const& stat = statIt->second;
        auto const name = stat[0];
        auto const type = stat[1];
        if (builder.name() != name) {
          foundError = true;
          LOG_TOPIC("f66dd", ERR, Logger::STATISTICS)
              << "Statistic '" << it.first << "' has mismatching names: '"
              << builder.name() << "' in statBuilder but '" << name
              << "' in statStrings";
        }
        if (builder.type() != type) {
          foundError = true;
          LOG_TOPIC("9fe22", ERR, Logger::STATISTICS)
              << "Statistic '" << it.first
              << "' has mismatching types (for API v2): '" << builder.type()
              << "' in statBuilder but '" << type << "' in statStrings";
        }
      }
    } else {
      foundError = true;
      LOG_TOPIC("015da", ERR, Logger::STATISTICS)
          << "Statistic '" << it.first
          << "' defined in statBuilder, but not in statStrings";
    }
  }
  for (auto const& it : statStrings) {
    auto const& statIt = statBuilder.find(it.first);
    if (statIt == statBuilder.end()) {
      foundError = true;
      LOG_TOPIC("eedac", ERR, Logger::STATISTICS)
          << "Statistic '" << it.first
          << "' defined in statStrings, but not in statBuilder";
    }
  }
  if (foundError) {
    FATAL_ERROR_EXIT();
  }
#endif
}

/*static*/ double StatisticsFeature::time() { return TRI_microtime(); }

void StatisticsFeature::collectOptions(
    std::shared_ptr<ProgramOptions> options) {
  options->addOldOption("server.disable-statistics", "server.statistics");

  options
      ->addOption("--server.statistics",
                  "Whether to enable statistics gathering and statistics APIs.",
                  new BooleanParameter(&_statistics))
      .setLongDescription(R"(If you set this option to `false`, then ArangoDB's
statistics gathering is turned off. Statistics gathering causes regular
background CPU activity, memory usage, and writes to the storage engine, so
using this option to turn statistics off might relieve heavily-loaded instances
a bit.

A side effect of setting this option to `false` is that no statistics are
shown in the dashboard of ArangoDB's web interface, and that the REST API for
server statistics at `/_admin/statistics` returns HTTP 404.)");

  options
      ->addOption("--server.statistics-history",
                  "Whether to store statistics in the database.",
                  new BooleanParameter(&_statisticsHistory),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Dynamic))
      .setLongDescription(R"(If you set this option to `false`, then ArangoDB's
statistics gathering is turned off. Statistics gathering causes regular
background CPU activity, memory usage, and writes to the storage engine, so
using this option to turn statistics off might relieve heavily-loaded instances
a bit.

If set to `false`, no statistics are shown in the dashboard of ArangoDB's
web interface, but the current statistics are available and can be queried
using the REST API for server statistics at `/_admin/statistics`.
This is less intrusive than setting the `--server.statistics` option to
`false`.)");

  options
      ->addOption(
          "--server.statistics-all-databases",
          "Provide cluster statistics in the web interface for all databases.",
          new BooleanParameter(&_statisticsAllDatabases),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30800);
}

void StatisticsFeature::validateOptions(
    std::shared_ptr<ProgramOptions> options) {
  if (_statistics) {
    // initialize counters for all HTTP request types
    ConnectionStatistics::initialize();
    RequestStatistics::initialize();
  } else {
    // turn ourselves off
    disable();
  }

  _statisticsHistoryTouched =
      options->processingResult().touched("--server.statistics-history");
}

void StatisticsFeature::start() {
  TRI_ASSERT(isEnabled());

  if (!server().hasFeature<arangodb::SystemDatabaseFeature>()) {
    LOG_TOPIC("9b551", FATAL, arangodb::Logger::STATISTICS)
        << "could not find feature 'SystemDatabase'";
    FATAL_ERROR_EXIT();
  }
  auto& sysDbFeature = server().getFeature<arangodb::SystemDatabaseFeature>();

  auto vocbase = sysDbFeature.use();

  if (!vocbase) {
    LOG_TOPIC("cff56", FATAL, arangodb::Logger::STATISTICS)
        << "could not find system database";
    FATAL_ERROR_EXIT();
  }

  // don't start the thread when we are running an upgrade
  auto& databaseFeature = server().getFeature<arangodb::DatabaseFeature>();
  if (!databaseFeature.upgrade()) {
    _statisticsThread = std::make_unique<StatisticsThread>(server());

    if (!_statisticsThread->start()) {
      LOG_TOPIC("46b0c", FATAL, arangodb::Logger::STATISTICS)
          << "could not start statistics thread";
      FATAL_ERROR_EXIT();
    }
  }

  // force history disable on Agents
  if (arangodb::ServerState::instance()->isAgent() &&
      !_statisticsHistoryTouched) {
    _statisticsHistory = false;
  }

  if (ServerState::instance()->isDBServer()) {
    // the StatisticsWorker runs queries against the _statistics
    // collections, so it does not work on DB servers
    _statisticsHistory = false;
  }

  if (_statisticsHistory) {
    _statisticsWorker = std::make_unique<StatisticsWorker>(*vocbase);

    if (!_statisticsWorker->start()) {
      LOG_TOPIC("6ecdc", FATAL, arangodb::Logger::STATISTICS)
          << "could not start statistics worker";
      FATAL_ERROR_EXIT();
    }
  }
}

void StatisticsFeature::stop() {
  if (_statisticsThread != nullptr) {
    _statisticsThread->beginShutdown();

    while (_statisticsThread->isRunning()) {
      std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
  }

  if (_statisticsWorker != nullptr) {
    _statisticsWorker->beginShutdown();

    while (_statisticsWorker->isRunning()) {
      std::this_thread::sleep_for(std::chrono::microseconds(10000));
    }
  }

  _statisticsThread.reset();
  _statisticsWorker.reset();
}

VPackBuilder StatisticsFeature::fillDistribution(
    statistics::Distribution const& dist) {
  VPackBuilder builder;
  builder.openObject();

  builder.add("sum", VPackValue(dist._total));
  builder.add("count", VPackValue(dist._count));

  builder.add("counts", VPackValue(VPackValueType::Array));
  for (auto const& val : dist._counts) {
    builder.add(VPackValue(val));
  }
  builder.close();

  builder.close();

  return builder;
}

void StatisticsFeature::appendHistogram(
    std::string& result, statistics::Distribution const& dist,
    std::string const& label, std::initializer_list<std::string> const& les,
    bool isInteger, std::string_view globals, bool ensureWhitespace) {
  VPackBuilder tmp = fillDistribution(dist);
  VPackSlice slc = tmp.slice();
  VPackSlice counts = slc.get("counts");

  auto const& stat = statStrings.at(label);
  auto const name = stat[0];
  auto const type = stat[1];
  auto const help = stat[2];

  metrics::Metric::addInfo(result, name, help, type);
  TRI_ASSERT(les.size() == counts.length());
  size_t i = 0;
  uint64_t sum = 0;
  for (auto const& le : les) {
    sum += counts.at(i++).getNumber<uint64_t>();
    absl::StrAppend(&result, name, "_bucket{le=\"", le, "\"",
                    (globals.empty() ? "" : ","), globals, "}",
                    (ensureWhitespace ? " " : ""), sum, "\n");
  }
  absl::StrAppend(&result, name, "_count{", globals, "}",
                  (ensureWhitespace ? " " : ""), sum, "\n");
  if (isInteger) {
    uint64_t v = slc.get("sum").getNumber<uint64_t>();
    absl::StrAppend(&result, name, "_sum{", globals, "}",
                    (ensureWhitespace ? " " : ""), v, "\n");
  } else {
    double v = slc.get("sum").getNumber<double>();
    // must use std::to_string() here because it produces a different
    // string representation of large floating-point numbers than absl
    // does. absl uses scientific notation for numbers that exceed 6
    // digits, and std::to_string() doesn't.
    absl::StrAppend(&result, name, "_sum{", globals, "}",
                    (ensureWhitespace ? " " : ""), std::to_string(v), "\n");
  }
}

void StatisticsFeature::appendMetric(std::string& result,
                                     std::string const& val,
                                     std::string const& label,
                                     std::string_view globals,
                                     bool ensureWhitespace) {
  auto const& stat = statStrings.at(label);
  auto const name = stat[0];
  auto const type = stat[1];
  auto const help = stat[2];

  metrics::Metric::addInfo(result, name, help, type);
  metrics::Metric::addMark(result, name, globals, "");
  absl::StrAppend(&result, ensureWhitespace ? " " : "", val, "\n");
}

void StatisticsFeature::toPrometheus(std::string& result, double now,
                                     std::string_view globals,
                                     bool ensureWhitespace) {
  // these metrics should always be 0 if statistics are disabled
  TRI_ASSERT(isEnabled() || (RequestStatistics::memoryUsage() == 0 &&
                             ConnectionStatistics::memoryUsage() == 0));
  if (isEnabled()) {
    // update arangodb_request_statistics_memory_usage and
    // arangodb_connection_statistics_memory_usage metrics
    _requestStatisticsMemoryUsage.store(RequestStatistics::memoryUsage(),
                                        std::memory_order_relaxed);
    _connectionStatisticsMemoryUsage.store(ConnectionStatistics::memoryUsage(),
                                           std::memory_order_relaxed);
  }

  ProcessInfo info = TRI_ProcessInfoSelf();
  uint64_t rss = static_cast<uint64_t>(info._residentSize);
  double rssp = 0;

  if (PhysicalMemory::getValue() != 0) {
    rssp = static_cast<double>(rss) /
           static_cast<double>(PhysicalMemory::getValue());
  }

  ServerStatistics const& serverInfo =
      server().getFeature<metrics::MetricsFeature>().serverStatistics();

  // processStatistics()
  appendMetric(result, std::to_string(info._minorPageFaults), "minorPageFaults",
               globals, ensureWhitespace);
  appendMetric(result, std::to_string(info._majorPageFaults), "majorPageFaults",
               globals, ensureWhitespace);
  if (info._scClkTck != 0) {  // prevent division by zero
    appendMetric(result,
                 std::to_string(static_cast<double>(info._userTime) /
                                static_cast<double>(info._scClkTck)),
                 "userTime", globals, ensureWhitespace);
    appendMetric(result,
                 std::to_string(static_cast<double>(info._systemTime) /
                                static_cast<double>(info._scClkTck)),
                 "systemTime", globals, ensureWhitespace);
  }
  appendMetric(result, std::to_string(info._numberThreads), "numberOfThreads",
               globals, ensureWhitespace);
  appendMetric(result, std::to_string(rss), "residentSize", globals,
               ensureWhitespace);
  appendMetric(result, std::to_string(rssp), "residentSizePercent", globals,
               ensureWhitespace);
  appendMetric(result, std::to_string(info._virtualSize), "virtualSize",
               globals, ensureWhitespace);
  appendMetric(result, std::to_string(PhysicalMemory::getValue()),
               "physicalSize", globals, ensureWhitespace);
  appendMetric(result, std::to_string(serverInfo.uptime()), "uptime", globals,
               ensureWhitespace);
  appendMetric(result, std::to_string(NumberOfCores::getValue()), "cores",
               globals, ensureWhitespace);

  CpuUsageFeature& cpuUsage = server().getFeature<CpuUsageFeature>();
  if (cpuUsage.isEnabled()) {
    auto snapshot = cpuUsage.snapshot();
    appendMetric(result, std::to_string(snapshot.userPercent()), "userPercent",
                 globals, ensureWhitespace);
    appendMetric(result, std::to_string(snapshot.systemPercent()),
                 "systemPercent", globals, ensureWhitespace);
    appendMetric(result, std::to_string(snapshot.idlePercent()), "idlePercent",
                 globals, ensureWhitespace);
    appendMetric(result, std::to_string(snapshot.iowaitPercent()),
                 "iowaitPercent", globals, ensureWhitespace);
  }

  if (isEnabled()) {
    ConnectionStatistics::Snapshot connectionStats;
    ConnectionStatistics::getSnapshot(connectionStats);

    RequestStatistics::Snapshot requestStats;
    RequestStatistics::getSnapshot(requestStats,
                                   stats::RequestStatisticsSource::ALL);

    // _clientStatistics()
    appendMetric(result, std::to_string(connectionStats.httpConnections.get()),
                 "clientHttpConnections", globals, ensureWhitespace);
    appendHistogram(result, connectionStats.connectionTime, "connectionTime",
                    {"0.01", "1.0", "60.0", "+Inf"}, false, globals,
                    ensureWhitespace);
    appendHistogram(result, requestStats.totalTime, "totalTime",
                    {"0.01", "0.05", "0.1", "0.2", "0.5", "1.0", "5.0", "15.0",
                     "30.0", "+Inf"},
                    false, globals, ensureWhitespace);
    appendHistogram(result, requestStats.requestTime, "requestTime",
                    {"0.01", "0.05", "0.1", "0.2", "0.5", "1.0", "5.0", "15.0",
                     "30.0", "+Inf"},
                    false, globals, ensureWhitespace);
    appendHistogram(result, requestStats.queueTime, "queueTime",
                    {"0.01", "0.05", "0.1", "0.2", "0.5", "1.0", "5.0", "15.0",
                     "30.0", "+Inf"},
                    false, globals, ensureWhitespace);
    appendHistogram(result, requestStats.ioTime, "ioTime",
                    {"0.01", "0.05", "0.1", "0.2", "0.5", "1.0", "5.0", "15.0",
                     "30.0", "+Inf"},
                    false, globals, ensureWhitespace);
    appendHistogram(result, requestStats.bytesSent, "bytesSent",
                    {"250", "1000", "2000", "5000", "10000", "+Inf"}, true,
                    globals, ensureWhitespace);
    appendHistogram(result, requestStats.bytesReceived, "bytesReceived",
                    {"250", "1000", "2000", "5000", "10000", "+Inf"}, true,
                    globals, ensureWhitespace);

    RequestStatistics::Snapshot requestStatsUser;
    RequestStatistics::getSnapshot(requestStatsUser,
                                   stats::RequestStatisticsSource::USER);
    appendHistogram(result, requestStatsUser.bytesSent, "bytesSentUser",
                    {"250", "1000", "2000", "5000", "10000", "+Inf"}, true,
                    globals, ensureWhitespace);
    appendHistogram(result, requestStatsUser.bytesReceived, "bytesReceivedUser",
                    {"250", "1000", "2000", "5000", "10000", "+Inf"}, true,
                    globals, ensureWhitespace);

    // _httpStatistics()
    using rest::RequestType;
    appendMetric(result, std::to_string(connectionStats.asyncRequests.get()),
                 "httpReqsAsync", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::DELETE_REQ].get()),
        "httpReqsDelete", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::GET].get()),
        "httpReqsGet", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::HEAD].get()),
        "httpReqsHead", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::OPTIONS].get()),
        "httpReqsOptions", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::PATCH].get()),
        "httpReqsPatch", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::POST].get()),
        "httpReqsPost", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::PUT].get()),
        "httpReqsPut", globals, ensureWhitespace);
    appendMetric(
        result,
        std::to_string(
            connectionStats.methodRequests[(int)RequestType::ILLEGAL].get()),
        "httpReqsOther", globals, ensureWhitespace);
    appendMetric(result, std::to_string(connectionStats.totalRequests.get()),
                 "httpReqsTotal", globals, ensureWhitespace);
    appendMetric(result,
                 std::to_string(connectionStats.totalRequestsSuperuser.get()),
                 "httpReqsSuperuser", globals, ensureWhitespace);
    appendMetric(result,
                 std::to_string(connectionStats.totalRequestsUser.get()),
                 "httpReqsUser", globals, ensureWhitespace);
  }

#ifdef USE_V8
  V8DealerFeature::Statistics v8Counters{};
  if (server().hasFeature<V8DealerFeature>()) {
    V8DealerFeature& dealer = server().getFeature<V8DealerFeature>();
    if (dealer.isEnabled()) {
      v8Counters = dealer.getCurrentExecutorStatistics();
    }
  }
  appendMetric(result, std::to_string(v8Counters.available),
               "v8ContextAvailable", globals, ensureWhitespace);
  appendMetric(result, std::to_string(v8Counters.busy), "v8ContextBusy",
               globals, ensureWhitespace);
  appendMetric(result, std::to_string(v8Counters.dirty), "v8ContextDirty",
               globals, ensureWhitespace);
  appendMetric(result, std::to_string(v8Counters.free), "v8ContextFree",
               globals, ensureWhitespace);
  appendMetric(result, std::to_string(v8Counters.min), "v8ContextMin", globals,
               ensureWhitespace);
  appendMetric(result, std::to_string(v8Counters.max), "v8ContextMax", globals,
               ensureWhitespace);
#else
  appendMetric(result, "0", "v8ContextAvailable", globals, ensureWhitespace);
  appendMetric(result, "0", "v8ContextBusy", globals, ensureWhitespace);
  appendMetric(result, "0", "v8ContextDirty", globals, ensureWhitespace);
  appendMetric(result, "0", "v8ContextFree", globals, ensureWhitespace);
  appendMetric(result, "0", "v8ContextMin", globals, ensureWhitespace);
  appendMetric(result, "0", "v8ContextMax", globals, ensureWhitespace);
#endif
}

Result StatisticsFeature::getClusterSystemStatistics(
    TRI_vocbase_t& vocbase, double start,
    arangodb::velocypack::Builder& result) const {
  if (!ServerState::instance()->isCoordinator()) {
    return {TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR};
  }

  if (!isEnabled()) {
    return {TRI_ERROR_DISABLED, "statistics are disabled"};
  }

  if (!vocbase.isSystem() && !_statisticsAllDatabases) {
    return {TRI_ERROR_FORBIDDEN,
            "statistics only available for system database"};
  }

  // we need to access the system database here...
  ExecContextSuperuserScope exscope;

  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();

  // build bind variables for query
  auto bindVars = std::make_shared<VPackBuilder>();

  auto buildBindVars = [&](std::string const& collection) {
    bindVars->clear();
    bindVars->openObject();
    bindVars->add("@collection", VPackValue(collection));
    bindVars->add("start", VPackValue(start));
    bindVars->add("clusterIds", VPackValue(VPackValueType::Array));
    for (auto const& coordinator : ci.getCurrentCoordinators()) {
      bindVars->add(VPackValue(coordinator));
    }
    bindVars->close();  // clusterIds
    bindVars->close();
  };

  auto origin =
      transaction::OperationOriginInternal{"retrieving cluster statistics"};

  auto& sysDbFeature = server().getFeature<arangodb::SystemDatabaseFeature>();
  auto sysVocbase = sysDbFeature.use();

  result.openObject();
  {
    buildBindVars(StaticStrings::Statistics15Collection);
    aql::QueryOptions options;
    options.cache = false;
    options.skipAudit = true;
    auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
        *sysVocbase, origin, aql::QueryString(stats15Query), bindVars,
        std::move(options));
    auto queryResult = std::move(queryFuture.waitAndGet());

    if (queryResult.result.fail()) {
      return queryResult.result;
    }

    result.add("stats15", queryResult.data->slice());
  }

  {
    buildBindVars(StaticStrings::StatisticsCollection);

    aql::QueryOptions options;
    options.cache = false;
    options.skipAudit = true;
    auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
        *sysVocbase, origin, aql::QueryString(statsSamplesQuery), bindVars,
        std::move(options));
    auto queryResult = std::move(queryFuture.waitAndGet());

    if (queryResult.result.fail()) {
      return queryResult.result;
    }

    result.add("statsSamples", queryResult.data->slice());
  }

  result.close();

  return {};
}
