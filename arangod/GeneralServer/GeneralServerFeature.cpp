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

#include "GeneralServerFeature.h"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Actions/RestActionHandler.h"
#include "Agency/AgencyFeature.h"
#include "Agency/RestAgencyHandler.h"
#include "Agency/RestAgencyPrivHandler.h"
#include "ApplicationFeatures/HttpEndpointProvider.h"
#include "Aql/RestAqlHandler.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Basics/FeatureFlags.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/LeaseManager/LeaseManager.h"
#include "Cluster/LeaseManager/LeaseManagerFeature.h"
#include "Cluster/LeaseManager/LeaseManagerRestHandler.h"
#include "Cluster/MaintenanceRestHandler.h"
#include "Cluster/RestAgencyCallbacksHandler.h"
#include "Cluster/RestClusterHandler.h"
#include "FeaturePhases/AqlFeaturePhase.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "GeneralServer/SslServerFeature.h"
#include "InternalRestHandler/InternalRestTraverserHandler.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/MetricsFeature.h"
#include "Network/NetworkFeature.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Rest/HttpResponse.h"
#include "RestHandler/RestAdminClusterHandler.h"
#include "RestHandler/RestAdminDatabaseHandler.h"
#ifdef USE_V8
#include "RestHandler/RestAdminExecuteHandler.h"
#endif
#include "RestHandler/RestAdminLogHandler.h"
#ifdef USE_V8
#include "RestHandler/RestAdminRoutingHandler.h"
#endif
#include "RestHandler/RestAdminServerHandler.h"
#include "RestHandler/RestAdminStatisticsHandler.h"
#include "RestHandler/RestAnalyzerHandler.h"
#include "RestHandler/RestAqlFunctionsHandler.h"
#ifdef USE_V8
#include "RestHandler/RestAqlUserFunctionsHandler.h"
#endif
#include "RestHandler/RestAuthHandler.h"
#include "RestHandler/RestAuthReloadHandler.h"
#include "RestHandler/RestBatchHandler.h"
#include "RestHandler/RestCompactHandler.h"
#include "RestHandler/RestCursorHandler.h"
#include "RestHandler/RestDatabaseHandler.h"
#include "RestHandler/RestDebugHandler.h"
#include "RestHandler/RestDocumentHandler.h"
#include "RestHandler/RestDocumentStateHandler.h"
#include "RestHandler/RestDumpHandler.h"
#include "RestHandler/RestEdgesHandler.h"
#include "RestHandler/RestEndpointHandler.h"
#include "RestHandler/RestEngineHandler.h"
#include "RestHandler/RestExplainHandler.h"
#include "RestHandler/RestGraphHandler.h"
#include "RestHandler/RestHandlerCreator.h"
#include "RestHandler/RestImportHandler.h"
#include "RestHandler/RestIndexHandler.h"
#include "RestHandler/RestKeyGeneratorsHandler.h"
#include "RestHandler/RestJobHandler.h"
#include "RestHandler/RestLicenseHandler.h"
#include "RestHandler/RestLogHandler.h"
#include "RestHandler/RestLogInternalHandler.h"
#include "RestHandler/RestMetricsHandler.h"
#include "RestHandler/RestOptionsDescriptionHandler.h"
#include "RestHandler/RestOptionsHandler.h"
#include "RestHandler/RestQueryCacheHandler.h"
#include "RestHandler/RestQueryHandler.h"
#include "RestHandler/RestShutdownHandler.h"
#include "RestHandler/RestSimpleHandler.h"
#include "RestHandler/RestSimpleQueryHandler.h"
#include "RestHandler/RestStatusHandler.h"
#include "RestHandler/RestSupervisionStateHandler.h"
#include "RestHandler/RestTelemetricsHandler.h"
#include "RestHandler/RestSupportInfoHandler.h"
#include "RestHandler/RestSystemReportHandler.h"
#include "RestHandler/RestTasksHandler.h"
#include "RestHandler/RestTestHandler.h"
#include "RestHandler/RestTimeHandler.h"
#include "RestHandler/RestTransactionHandler.h"
#include "RestHandler/RestTtlHandler.h"
#include "RestHandler/RestUploadHandler.h"
#include "RestHandler/RestUsageMetricsHandler.h"
#include "RestHandler/RestUsersHandler.h"
#include "RestHandler/RestVersionHandler.h"
#include "RestHandler/RestViewHandler.h"
#include "RestHandler/RestWalAccessHandler.h"
#include "RestServer/EndpointFeature.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/GaugeBuilder.h"
#include "Metrics/MetricsFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/UpgradeFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif

#ifdef USE_ENTERPRISE
#include "Enterprise/RestHandler/RestHotBackupHandler.h"
#include "Enterprise/StorageEngine/HotBackupFeature.h"
#endif

using namespace arangodb::rest;
using namespace arangodb::options;

namespace arangodb {

struct RequestBodySizeScale {
  static metrics::LogScale<uint64_t> scale() { return {2, 64, 65536, 10}; }
};

DECLARE_HISTOGRAM(arangodb_request_body_size_http1, RequestBodySizeScale,
                  "Body size of HTTP/1.1 requests");
DECLARE_HISTOGRAM(arangodb_request_body_size_http2, RequestBodySizeScale,
                  "Body size of HTTP/2 requests");
DECLARE_COUNTER(arangodb_http1_connections_total,
                "Total number of HTTP/1.1 connections");
DECLARE_COUNTER(arangodb_http2_connections_total,
                "Total number of HTTP/2 connections");
DECLARE_GAUGE(arangodb_requests_memory_usage, std::uint64_t,
              "Memory consumed by incoming requests");

GeneralServerFeature::GeneralServerFeature(Server& server,
                                           metrics::MetricsFeature& metrics)
    : ArangodFeature{server, *this},
      _currentRequestsSize(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_requests_memory_usage{})),
      _telemetricsMaxRequestsPerInterval(3),
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      _startedListening(false),
#endif
      _allowEarlyConnections(false),
      _handleContentEncodingForUnauthenticatedRequests(false),
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      _enableTelemetrics(false),
#else
      _enableTelemetrics(true),
#endif
      _proxyCheck(true),
      _returnQueueTimeHeader(true),
      _permanentRootRedirect(true),
      _compressResponseThreshold(0),
      _redirectRootTo("/_admin/aardvark/index.html"),
      _supportInfoApiPolicy("admin"),
      _optionsApiPolicy("jwt"),
      _numIoThreads(NetworkFeature::defaultIOThreads()),
      _requestBodySizeHttp1(metrics.add(arangodb_request_body_size_http1{})),
      _requestBodySizeHttp2(metrics.add(arangodb_request_body_size_http2{})),
      _http1Connections(metrics.add(arangodb_http1_connections_total{})),
      _http2Connections(metrics.add(arangodb_http2_connections_total{})) {
  static_assert(
      Server::isCreatedAfter<GeneralServerFeature, metrics::MetricsFeature>());

  setOptional(true);
  startsAfter<application_features::AqlFeaturePhase>();

  startsAfter<HttpEndpointProvider>();
  startsAfter<SslServerFeature>();
  startsAfter<SchedulerFeature>();
  startsAfter<UpgradeFeature>();
}

void GeneralServerFeature::collectOptions(
    std::shared_ptr<ProgramOptions> options) {
  options->addOldOption("server.allow-method-override",
                        "http.allow-method-override");
  options->addOldOption("server.hide-product-header",
                        "http.hide-product-header");
  options->addOldOption("server.keep-alive-timeout", "http.keep-alive-timeout");
  options->addOldOption("no-server", "server.rest-server");

  options
      ->addOption("--server.telemetrics-api",
                  "Whether to enable the telemetrics API.",
                  new options::BooleanParameter(&_enableTelemetrics),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::Uncommon,
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator,
                      arangodb::options::Flags::OnDBServer,
                      arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100);

  options
      ->addOption(
          "--server.telemetrics-api-max-requests",
          "The maximum number of requests from arangosh that the "
          "telemetrics API responds to without rate-limiting.",
          new options::UInt64Parameter(&_telemetricsMaxRequestsPerInterval),
          arangodb::options::makeFlags(
              arangodb::options::Flags::Uncommon,
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnCoordinator,
              arangodb::options::Flags::OnSingle))
      .setIntroducedIn(31100)
      .setLongDescription(R"(This option limits requests from the arangosh to
the telemetrics API, but not any other requests to the API.

Requests to the telemetrics API are counted for every 2 hour interval, and then
reset. This means after a period of at most 2 hours, the telemetrics API
becomes usable again.

The purpose of this option is to keep a deployment from being overwhelmed by
too many telemetrics requests issued by arangosh instances that are used for
batch processing.)");

  options->addOption(
      "--server.io-threads", "The number of threads used to handle I/O.",
      new UInt64Parameter(&_numIoThreads, /*base*/ 1, /*minValue*/ 1),
      arangodb::options::makeDefaultFlags(arangodb::options::Flags::Dynamic));

  options
      ->addOption("--server.support-info-api",
                  "The policy for exposing the support info and also the "
                  "telemetrics API.",
                  new DiscreteValuesParameter<StringParameter>(
                      &_supportInfoApiPolicy,
                      std::unordered_set<std::string>{"disabled", "jwt",
                                                      "admin", "public"}))
      .setIntroducedIn(30900);

  options
      ->addOption("--server.options-api",
                  "The policy for exposing the options API.",
                  new DiscreteValuesParameter<StringParameter>(
                      &_optionsApiPolicy,
                      std::unordered_set<std::string>{"disabled", "jwt",
                                                      "admin", "public"}))
      .setIntroducedIn(31200);

  options->addSection("http", "HTTP server features");

  // option was deprecated in 3.8 and removed in 3.12.
  options->addObsoleteOption(
      "--http.allow-method-override",
      "Allow HTTP method override using special headers.", true);

  options
      ->addOption("--http.keep-alive-timeout",
                  "The keep-alive timeout for HTTP connections (in seconds).",
                  new DoubleParameter(&_keepAliveTimeout))
      .setLongDescription(R"(Idle keep-alive connections are closed by the
server automatically when the timeout is reached. A keep-alive-timeout value of
`0` disables the keep-alive feature entirely.)");

  // option was deprecated in 3.8 and removed in 3.12.
  options->addObsoleteOption(
      "--http.hide-product-header",
      "Whether to omit the `Server: ArangoDB` header in HTTP responses.", true);

  options->addOption(
      "--http.trusted-origin",
      "The trusted origin URLs for CORS requests with credentials.",
      new VectorParameter<StringParameter>(&_accessControlAllowOrigins));

  options->addOption("--http.redirect-root-to", "Redirect of the root URL.",
                     new StringParameter(&_redirectRootTo));

  options->addOption("--http.permanently-redirect-root",
                     "Whether to use a permanent or temporary redirect.",
                     new BooleanParameter(&_permanentRootRedirect));

  options
      ->addOption("--http.return-queue-time-header",
                  "Whether to return the `x-arango-queue-time-seconds` header "
                  "in all responses.",
                  new BooleanParameter(&_returnQueueTimeHeader))
      .setIntroducedIn(30900)
      .setLongDescription(R"(The value contained in this header indicates the
current queueing/dequeuing time for requests in the scheduler (in seconds).
Client applications and drivers can use this value to control the server load
and also react on overload.)");

  options
      ->addOption("--http.compress-response-threshold",
                  "The HTTP response body size from which on responses are "
                  "transparently compressed in case the client asks for it.",
                  new UInt64Parameter(&_compressResponseThreshold))
      .setIntroducedIn(31200)
      .setLongDescription(
          R"(Automatically compress outgoing HTTP responses with the
deflate or gzip compression format, in case the client request advertises
support for this. Compression will only happen for HTTP/1.1 and HTTP/2
connections, if the size of the uncompressed response body exceeds 
the threshold value controlled by this startup option,
and if the response body size after compression is less than the original 
response body size.
Using the value 0 disables the automatic response compression.")");

  options
      ->addOption("--server.early-connections",
                  "Allow requests to a limited set of APIs early during the "
                  "server startup.",
                  new BooleanParameter(&_allowEarlyConnections))
      .setIntroducedIn(31000);

  options->addOldOption("frontend.proxy-request-check",
                        "web-interface.proxy-request-check");

  options->addOption("--web-interface.proxy-request-check",
                     "Enable proxy request checking.",
                     new BooleanParameter(&_proxyCheck),
                     arangodb::options::makeFlags(
                         arangodb::options::Flags::DefaultNoComponents,
                         arangodb::options::Flags::OnCoordinator,
                         arangodb::options::Flags::OnSingle));

  options->addOldOption("frontend.trusted-proxy",
                        "web-interface.trusted-proxy");

  options->addOption(
      "--web-interface.trusted-proxy",
      "The list of proxies to trust (can be IP or network). Make "
      "sure `--web-interface.proxy-request-check` is enabled.",
      new VectorParameter<StringParameter>(&_trustedProxies),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnCoordinator,
          arangodb::options::Flags::OnSingle));

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  options->addOption(
      "--server.failure-point",
      "The failure point to set during server startup (requires compilation "
      "with failure points support).",
      new VectorParameter<StringParameter>(&_failurePoints),
      arangodb::options::makeFlags(arangodb::options::Flags::Default,
                                   arangodb::options::Flags::Uncommon));
#endif

  options
      ->addOption(
          "--http.handle-content-encoding-for-unauthenticated-requests",
          "Handle Content-Encoding headers for unauthenticated requests.",
          new BooleanParameter(
              &_handleContentEncodingForUnauthenticatedRequests))
      .setIntroducedIn(31200)
      .setLongDescription(
          R"(If the option is set to `true`, the server will automatically 
uncompress incoming HTTP requests with Content-Encodings gzip and deflate
even if the request is not authenticated.)");
}

void GeneralServerFeature::validateOptions(std::shared_ptr<ProgramOptions>) {
  if (!_accessControlAllowOrigins.empty()) {
    // trim trailing slash from all members
    for (auto& it : _accessControlAllowOrigins) {
      if (it == "*" || it == "all") {
        // special members "*" or "all" means all origins are allowed
        _accessControlAllowOrigins.clear();
        _accessControlAllowOrigins.push_back("*");
        break;
      } else if (it == "none") {
        // "none" means no origins are allowed
        _accessControlAllowOrigins.clear();
        break;
      } else if (it.ends_with('/')) {
        // strip trailing slash
        it = it.substr(0, it.size() - 1);
      }
    }

    // remove empty members
    _accessControlAllowOrigins.erase(
        std::remove_if(_accessControlAllowOrigins.begin(),
                       _accessControlAllowOrigins.end(),
                       [](std::string const& value) {
                         return basics::StringUtils::trim(value).empty();
                       }),
        _accessControlAllowOrigins.end());
  }

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  for (auto const& it : _failurePoints) {
    TRI_AddFailurePointDebugging(it);
  }
#endif
}

void GeneralServerFeature::prepare() {
  ServerState::instance()->setServerMode(ServerState::Mode::STARTUP);

  if (ServerState::instance()->isAgent()) {
    // telemetrics automatically and always turned off on agents
    _enableTelemetrics = false;
  }

  _jobManager = std::make_unique<AsyncJobManager>();

  // create an initial, very stripped-down RestHandlerFactory.
  // this initial factory only knows a few selected RestHandlers.
  // we will later create another RestHandlerFactory that knows
  // all routes.
  auto hf = std::make_shared<RestHandlerFactory>();
  defineInitialHandlers(*hf);
  // make handler-factory read-only
  hf->seal();

  std::atomic_store(&_handlerFactory, std::move(hf));

  buildServers();

  if (_allowEarlyConnections) {
    // open HTTP interface early if this is requested.
    startListening();
  }

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  TRI_IF_FAILURE("startListeningEarly") {
    while (TRI_ShouldFailDebugging("startListeningEarly")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
#endif
}

void GeneralServerFeature::start() {
  TRI_ASSERT(ServerState::instance()->mode() == ServerState::Mode::STARTUP);

  // create the full RestHandlerFactory that knows all the routes.
  // this will replace the previous, stripped-down RestHandlerFactory
  // instance.
  auto hf = std::make_shared<RestHandlerFactory>();

  defineInitialHandlers(*hf);
  defineRemainingHandlers(*hf);
  hf->seal();

  std::atomic_store(&_handlerFactory, std::move(hf));

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(!_allowEarlyConnections || _startedListening);
#endif
  if (!_allowEarlyConnections) {
    // if HTTP interface is not open yet, open it now
    startListening();
  }
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(_startedListening);
#endif

  ServerState::instance()->setServerMode(ServerState::Mode::MAINTENANCE);
}

void GeneralServerFeature::initiateSoftShutdown() {
  if (_jobManager != nullptr) {
    _jobManager->initiateSoftShutdown();
  }
}

void GeneralServerFeature::beginShutdown() {
  for (auto& server : _servers) {
    server->stopListening();
  }
}

void GeneralServerFeature::stop() {
  _jobManager->deleteJobs();
  for (auto& server : _servers) {
    server->stopConnections();
  }
}

void GeneralServerFeature::unprepare() {
  for (auto& server : _servers) {
    server->stopWorking();
  }
  _servers.clear();
  _jobManager.reset();
}

double GeneralServerFeature::keepAliveTimeout() const noexcept {
  return _keepAliveTimeout;
}

bool GeneralServerFeature::handleContentEncodingForUnauthenticatedRequests()
    const noexcept {
  return _handleContentEncodingForUnauthenticatedRequests;
}

bool GeneralServerFeature::proxyCheck() const noexcept { return _proxyCheck; }

bool GeneralServerFeature::returnQueueTimeHeader() const noexcept {
  return _returnQueueTimeHeader;
}

std::vector<std::string> GeneralServerFeature::trustedProxies() const {
  return _trustedProxies;
}

std::vector<std::string> const&
GeneralServerFeature::accessControlAllowOrigins() const {
  return _accessControlAllowOrigins;
}

Result GeneralServerFeature::reloadTLS() {  // reload TLS data from disk
  Result res;
  for (auto& up : _servers) {
    Result res2 = up->reloadTLS();
    if (res2.fail()) {
      res = res2;  // yes, we only report the last error if there is one
    }
  }
  return res;
}

bool GeneralServerFeature::permanentRootRedirect() const noexcept {
  return _permanentRootRedirect;
}

std::string GeneralServerFeature::redirectRootTo() const {
  return _redirectRootTo;
}

std::string const& GeneralServerFeature::supportInfoApiPolicy() const noexcept {
  return _supportInfoApiPolicy;
}

std::string const& GeneralServerFeature::optionsApiPolicy() const noexcept {
  return _optionsApiPolicy;
}

uint64_t GeneralServerFeature::compressResponseThreshold() const noexcept {
  return _compressResponseThreshold;
}

std::shared_ptr<rest::RestHandlerFactory> GeneralServerFeature::handlerFactory()
    const {
  return std::atomic_load_explicit(&_handlerFactory, std::memory_order_relaxed);
}

rest::AsyncJobManager& GeneralServerFeature::jobManager() {
  return *_jobManager;
}

void GeneralServerFeature::buildServers() {
  EndpointFeature& endpoint =
      server().getFeature<HttpEndpointProvider, EndpointFeature>();
  auto const& endpointList = endpoint.endpointList();

  // check if endpointList contains ssl featured server
  if (endpointList.hasSsl()) {
    if (!server().hasFeature<SslServerFeature>()) {
      LOG_TOPIC("8df10", FATAL, arangodb::Logger::FIXME)
          << "no ssl context is known, cannot create https server, "
             "please enable SSL";
      FATAL_ERROR_EXIT();
    }
    SslServerFeature& ssl = server().getFeature<SslServerFeature>();
    ssl.verifySslOptions();
  }

  _servers.emplace_back(std::make_unique<GeneralServer>(
      *this, _numIoThreads, _allowEarlyConnections));
}

void GeneralServerFeature::startListening() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(!_startedListening);
#endif

  EndpointFeature& endpoint =
      server().getFeature<HttpEndpointProvider, EndpointFeature>();
  auto& endpointList = endpoint.endpointList();

  for (auto& server : _servers) {
    server->startListening(endpointList);
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  _startedListening = true;
#endif
}

void GeneralServerFeature::defineInitialHandlers(rest::RestHandlerFactory& f) {
  // these handlers will be available early during the server start.
  // if you want to add more handlers here, please make sure that they
  // run on the CLIENT_FAST request lane. otherwise the incoming requests
  // will still be rejected during startup, even though they are registered
  // here.
  f.addHandler("/_api/version",
               RestHandlerCreator<RestVersionHandler>::createNoData);
  f.addHandler("/_admin/version",
               RestHandlerCreator<RestVersionHandler>::createNoData);
  f.addHandler("/_admin/status",
               RestHandlerCreator<RestStatusHandler>::createNoData);
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  // This handler can be used to control failure points
  f.addPrefixHandler(
      "/_admin/debug",
      RestHandlerCreator<arangodb::RestDebugHandler>::createNoData);
#endif
}

void GeneralServerFeature::defineRemainingHandlers(
    rest::RestHandlerFactory& f) {
  TRI_ASSERT(_jobManager != nullptr);

  AgencyFeature& agency = server().getFeature<AgencyFeature>();
  ClusterFeature& cluster = server().getFeature<ClusterFeature>();
  AuthenticationFeature& authentication =
      server().getFeature<AuthenticationFeature>();

  // ...........................................................................
  // /_api
  // ...........................................................................

  f.addPrefixHandler(                         // add handler
      RestVocbaseBaseHandler::ANALYZER_PATH,  // base URL
      RestHandlerCreator<
          iresearch::RestAnalyzerHandler>::createNoData  // handler
  );

  f.addPrefixHandler(RestVocbaseBaseHandler::BATCH_PATH,
                     RestHandlerCreator<RestBatchHandler>::createNoData);

  auto queryRegistry = QueryRegistryFeature::registry();
  f.addPrefixHandler(
      RestVocbaseBaseHandler::CURSOR_PATH,
      RestHandlerCreator<RestCursorHandler>::createData<aql::QueryRegistry*>,
      queryRegistry);

  f.addPrefixHandler(RestVocbaseBaseHandler::DATABASE_PATH,
                     RestHandlerCreator<RestDatabaseHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::DOCUMENT_PATH,
                     RestHandlerCreator<RestDocumentHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::EDGES_PATH,
                     RestHandlerCreator<RestEdgesHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::GHARIAL_PATH,
                     RestHandlerCreator<RestGraphHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::ENDPOINT_PATH,
                     RestHandlerCreator<RestEndpointHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::IMPORT_PATH,
                     RestHandlerCreator<RestImportHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::INDEX_PATH,
                     RestHandlerCreator<RestIndexHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::SIMPLE_QUERY_ALL_PATH,
                     RestHandlerCreator<RestSimpleQueryHandler>::createData<
                         aql::QueryRegistry*>,
                     queryRegistry);

  f.addPrefixHandler(RestVocbaseBaseHandler::SIMPLE_QUERY_ALL_KEYS_PATH,
                     RestHandlerCreator<RestSimpleQueryHandler>::createData<
                         aql::QueryRegistry*>,
                     queryRegistry);

  f.addPrefixHandler(RestVocbaseBaseHandler::SIMPLE_QUERY_BY_EXAMPLE,
                     RestHandlerCreator<RestSimpleQueryHandler>::createData<
                         aql::QueryRegistry*>,
                     queryRegistry);

  f.addPrefixHandler(
      RestVocbaseBaseHandler::SIMPLE_LOOKUP_PATH,
      RestHandlerCreator<RestSimpleHandler>::createData<aql::QueryRegistry*>,
      queryRegistry);

  f.addPrefixHandler(
      RestVocbaseBaseHandler::SIMPLE_REMOVE_PATH,
      RestHandlerCreator<RestSimpleHandler>::createData<aql::QueryRegistry*>,
      queryRegistry);

#ifdef USE_V8
  if (server().isEnabled<V8DealerFeature>()) {
    // the tasks feature depends on V8. only enable it if JavaScript is enabled
    f.addPrefixHandler(RestVocbaseBaseHandler::TASKS_PATH,
                       RestHandlerCreator<RestTasksHandler>::createNoData);
  }
#endif

  f.addPrefixHandler(RestVocbaseBaseHandler::UPLOAD_PATH,
                     RestHandlerCreator<RestUploadHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::USERS_PATH,
                     RestHandlerCreator<RestUsersHandler>::createNoData);

  f.addPrefixHandler(RestVocbaseBaseHandler::VIEW_PATH,
                     RestHandlerCreator<RestViewHandler>::createNoData);

  if (::arangodb::replication2::EnableReplication2 && cluster.isEnabled()) {
    f.addPrefixHandler(std::string{StaticStrings::ApiLogExternal},
                       RestHandlerCreator<RestLogHandler>::createNoData);
    f.addPrefixHandler(
        std::string{StaticStrings::ApiLogInternal},
        RestHandlerCreator<RestLogInternalHandler>::createNoData);
    f.addPrefixHandler(
        std::string{StaticStrings::ApiDocumentStateExternal},
        RestHandlerCreator<RestDocumentStateHandler>::createNoData);
  }

  // This is the only handler were we need to inject
  // more than one data object. So we created the combinedRegistries
  // for it.
  f.addPrefixHandler(
      "/_api/aql",
      RestHandlerCreator<aql::RestAqlHandler>::createData<aql::QueryRegistry*>,
      queryRegistry);

  f.addPrefixHandler("/_api/aql-builtin",
                     RestHandlerCreator<RestAqlFunctionsHandler>::createNoData);

#ifdef USE_V8
  if (server().isEnabled<V8DealerFeature>()) {
    // the AQL UDfs feature depends on V8. only enable it if JavaScript is
    // enabled
    f.addPrefixHandler(
        "/_api/aqlfunction",
        RestHandlerCreator<RestAqlUserFunctionsHandler>::createNoData);
  }
#endif

  f.addPrefixHandler(
      "/_api/dump",
      RestHandlerCreator<arangodb::RestDumpHandler>::createNoData);

  f.addPrefixHandler("/_api/explain",
                     RestHandlerCreator<RestExplainHandler>::createNoData);

  f.addPrefixHandler(
      "/_api/key-generators",
      RestHandlerCreator<RestKeyGeneratorsHandler>::createNoData);

  f.addPrefixHandler("/_api/query",
                     RestHandlerCreator<RestQueryHandler>::createNoData);

  f.addPrefixHandler("/_api/query-cache",
                     RestHandlerCreator<RestQueryCacheHandler>::createNoData);

  f.addPrefixHandler("/_api/wal",
                     RestHandlerCreator<RestWalAccessHandler>::createNoData);

  if (agency.isEnabled()) {
    f.addPrefixHandler(
        RestVocbaseBaseHandler::AGENCY_PATH,
        RestHandlerCreator<RestAgencyHandler>::createData<consensus::Agent*>,
        agency.agent());

    f.addPrefixHandler(RestVocbaseBaseHandler::AGENCY_PRIV_PATH,
                       RestHandlerCreator<RestAgencyPrivHandler>::createData<
                           consensus::Agent*>,
                       agency.agent());
  }

  if (cluster.isEnabled()) {
    // add "/agency-callbacks" handler
    f.addPrefixHandler(
        cluster.agencyCallbacksPath(),
        RestHandlerCreator<RestAgencyCallbacksHandler>::createData<
            AgencyCallbackRegistry*>,
        cluster.agencyCallbackRegistry());
    // add "_api/cluster" handler
    f.addPrefixHandler(cluster.clusterRestPath(),
                       RestHandlerCreator<RestClusterHandler>::createNoData);
  }
  f.addPrefixHandler(
      RestVocbaseBaseHandler::INTERNAL_TRAVERSER_PATH,
      RestHandlerCreator<InternalRestTraverserHandler>::createData<
          aql::QueryRegistry*>,
      queryRegistry);

  // And now some handlers which are registered in both /_api and /_admin
  f.addHandler("/_admin/actions",
               RestHandlerCreator<MaintenanceRestHandler>::createNoData);

  f.addHandler("/_admin/auth/reload",
               RestHandlerCreator<RestAuthReloadHandler>::createNoData);

#ifdef USE_V8
  if (server().hasFeature<V8DealerFeature>() &&
      server().getFeature<V8DealerFeature>().allowAdminExecute()) {
    // the /_admin/execute API depends on V8. only enable it if JavaScript is
    // enabled
    f.addHandler("/_admin/execute",
                 RestHandlerCreator<RestAdminExecuteHandler>::createNoData);
  }
#endif

  f.addHandler("/_admin/time",
               RestHandlerCreator<RestTimeHandler>::createNoData);

  f.addHandler("/_admin/compact",
               RestHandlerCreator<RestCompactHandler>::createNoData);

  f.addPrefixHandler("/_api/job",
                     RestHandlerCreator<arangodb::RestJobHandler>::createData<
                         AsyncJobManager*>,
                     _jobManager.get());

  f.addPrefixHandler("/_api/engine",
                     RestHandlerCreator<RestEngineHandler>::createNoData);

  f.addPrefixHandler("/_api/transaction",
                     RestHandlerCreator<RestTransactionHandler>::createNoData);

  f.addPrefixHandler("/_api/ttl",
                     RestHandlerCreator<RestTtlHandler>::createNoData);

  // ...........................................................................
  // /_admin
  // ...........................................................................

  f.addPrefixHandler(
      "/_admin/cluster",
      RestHandlerCreator<arangodb::RestAdminClusterHandler>::createNoData);

  if (_supportInfoApiPolicy != "disabled") {
    f.addHandler("/_admin/support-info",
                 RestHandlerCreator<RestSupportInfoHandler>::createNoData);

    f.addHandler("/_admin/telemetrics",
                 RestHandlerCreator<RestTelemetricsHandler>::createNoData);
  }

  if (_optionsApiPolicy != "disabled") {
    f.addHandler("/_admin/options",
                 RestHandlerCreator<RestOptionsHandler>::createNoData);
    f.addHandler(
        "/_admin/options-description",
        RestHandlerCreator<RestOptionsDescriptionHandler>::createNoData);
  }

  f.addHandler("/_admin/system-report",
               RestHandlerCreator<RestSystemReportHandler>::createNoData);

  f.addPrefixHandler("/_admin/job",
                     RestHandlerCreator<arangodb::RestJobHandler>::createData<
                         AsyncJobManager*>,
                     _jobManager.get());

  // further admin handlers
  f.addPrefixHandler(
      "/_admin/database/target-version",
      RestHandlerCreator<arangodb::RestAdminDatabaseHandler>::createNoData);

  f.addPrefixHandler(
      "/_admin/log",
      RestHandlerCreator<arangodb::RestAdminLogHandler>::createNoData);

#ifdef USE_V8
  if (server().isEnabled<V8DealerFeature>()) {
    // the routing feature depends on V8. only enable it if JavaScript is
    // enabled
    f.addPrefixHandler(
        "/_admin/routing",
        RestHandlerCreator<arangodb::RestAdminRoutingHandler>::createNoData);
  }
#endif

  f.addHandler(
      "/_admin/supervisionState",
      RestHandlerCreator<arangodb::RestSupervisionStateHandler>::createNoData);

  f.addPrefixHandler(
      "/_admin/shutdown",
      RestHandlerCreator<arangodb::RestShutdownHandler>::createNoData);

  if (authentication.isActive()) {
    f.addPrefixHandler(
        "/_open/auth",
        RestHandlerCreator<arangodb::RestAuthHandler>::createNoData);
  }

  f.addPrefixHandler(
      "/_admin/server",
      RestHandlerCreator<arangodb::RestAdminServerHandler>::createNoData);

  f.addHandler(
      "/_admin/statistics",
      RestHandlerCreator<arangodb::RestAdminStatisticsHandler>::createNoData);

  f.addPrefixHandler(
      "/_admin/metrics",
      RestHandlerCreator<arangodb::RestMetricsHandler>::createNoData);

  f.addPrefixHandler(
      "/_admin/usage-metrics",
      RestHandlerCreator<arangodb::RestUsageMetricsHandler>::createNoData);

  f.addHandler(
      "/_admin/statistics-description",
      RestHandlerCreator<arangodb::RestAdminStatisticsHandler>::createNoData);

  f.addPrefixHandler(
      "/_admin/license",
      RestHandlerCreator<arangodb::RestLicenseHandler>::createNoData);

  {
    auto& leaseManagerFeature =
        server().getFeature<cluster::LeaseManagerFeature>();
    f.addPrefixHandler(
        "/_admin/leases",
        RestHandlerCreator<arangodb::cluster::LeaseManagerRestHandler>::
            createData<cluster::LeaseManager*>,
        &leaseManagerFeature.leaseManager());
  }

#ifdef USE_ENTERPRISE
  HotBackupFeature& backup = server().getFeature<HotBackupFeature>();
  if (backup.isAPIEnabled()) {
    f.addPrefixHandler(
        "/_admin/backup",
        RestHandlerCreator<arangodb::RestHotBackupHandler>::createNoData);
  }
#endif

  // ...........................................................................
  // test handler
  // ...........................................................................
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  f.addPrefixHandler("/_api/test",
                     RestHandlerCreator<RestTestHandler>::createNoData);
#endif

  // ...........................................................................
  // actions defined in v8
  // ...........................................................................

  f.addPrefixHandler("/", RestHandlerCreator<RestActionHandler>::createNoData);

  // engine specific handlers
  StorageEngine& engine = server().getFeature<EngineSelectorFeature>().engine();
  engine.addRestHandlers(f);
}

}  // namespace arangodb
