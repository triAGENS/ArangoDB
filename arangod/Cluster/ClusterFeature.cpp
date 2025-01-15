////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ClusterFeature.h"

#include "Agency/AsyncAgencyComm.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FileUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/application-exit.h"
#include "Basics/files.h"
#include "Basics/system-functions.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/HeartbeatThread.h"
#include "Endpoint/Endpoint.h"
#include "Futures/Utilities.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Random/RandomGenerator.h"
#include "RestServer/DatabaseFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/HistogramBuilder.h"
#include "Metrics/LogScale.h"
#include "Metrics/MetricsFeature.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

struct ClusterFeatureScale {
  static metrics::LogScale<uint64_t> scale() { return {2, 58, 120000, 10}; }
};

DECLARE_HISTOGRAM(arangodb_agencycomm_request_time_msec, ClusterFeatureScale,
                  "Request time for Agency requests [ms]");

ClusterFeature::ClusterFeature(Server& server)
    : ArangodFeature{server, *this},
      _apiJwtPolicy("jwt-compat"),
      _metrics{server.getFeature<metrics::MetricsFeature>()},
      _agency_comm_request_time_ms(
          _metrics.add(arangodb_agencycomm_request_time_msec{})) {
  static_assert(
      Server::isCreatedAfter<ClusterFeature, metrics::MetricsFeature>());

  setOptional(true);
  startsAfter<application_features::CommunicationFeaturePhase>();
  startsAfter<application_features::DatabaseFeaturePhase>();
}

ClusterFeature::~ClusterFeature() { shutdown(); }

void ClusterFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("cluster", "cluster");

  options->addObsoleteOption("--cluster.username",
                             "username used for cluster-internal communication",
                             true);
  options->addObsoleteOption("--cluster.password",
                             "password used for cluster-internal communication",
                             true);
  options->addObsoleteOption("--cluster.disable-dispatcher-kickstarter",
                             "The dispatcher feature isn't available anymore; "
                             "Use ArangoDBStarter for this now!",
                             true);
  options->addObsoleteOption("--cluster.disable-dispatcher-frontend",
                             "The dispatcher feature isn't available anymore; "
                             "Use ArangoDB Starter for this now!",
                             true);
  options->addObsoleteOption(
      "--cluster.dbserver-config",
      "The dbserver-config is not available anymore, Use ArangoDBStarter",
      true);
  options->addObsoleteOption(
      "--cluster.coordinator-config",
      "The coordinator-config is not available anymore, Use ArangoDBStarter",
      true);
  options->addObsoleteOption("--cluster.data-path",
                             "path to cluster database directory", true);
  options->addObsoleteOption("--cluster.log-path",
                             "path to log directory for the cluster", true);
  options->addObsoleteOption("--cluster.arangod-path",
                             "path to the arangod for the cluster", true);
  options->addObsoleteOption("--cluster.my-local-info",
                             "this server's local info", false);
  options->addObsoleteOption("--cluster.my-id", "this server's id", false);

  options->addObsoleteOption("--cluster.agency-prefix", "agency prefix", false);

  options->addOption(
      "--cluster.require-persisted-id",
      "If set to `true`, then the instance only starts if a UUID file is found "
      "in the database directory on startup. This ensures that the instance is "
      "started using an already existing database directory and not a new one. "
      "For the first start, you must either create the UUID file manually or "
      "set the option to `false` for the initial startup.",
      new BooleanParameter(&_requirePersistedId));

  options
      ->addOption("--cluster.agency-endpoint",
                  "Agency endpoint(s) to connect to.",
                  new VectorParameter<StringParameter>(&_agencyEndpoints),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator,
                      arangodb::options::Flags::OnDBServer))
      .setLongDescription(R"(You can specify this option multiple times to let
the server use a cluster of Agency servers.

Endpoints have the following pattern:

- `tcp://ipv4-address:port` - TCP/IP endpoint, using IPv4
- `tcp://[ipv6-address]:port` - TCP/IP endpoint, using IPv6
- `ssl://ipv4-address:port` - TCP/IP endpoint, using IPv4, SSL encryption
- `ssl://[ipv6-address]:port` - TCP/IP endpoint, using IPv6, SSL encryption

You must specify at least one endpoint or ArangoDB refuses to start. It is
recommended to specify at least two endpoints, so that ArangoDB has an
alternative endpoint if one of them becomes unavailable:

`--cluster.agency-endpoint tcp://192.168.1.1:4001
--cluster.agency-endpoint tcp://192.168.1.2:4002 ...`)");

  options
      ->addOption("--cluster.my-role", "This server's role.",
                  new StringParameter(&_myRole))
      .setLongDescription(R"(For a cluster, the possible values are `DBSERVER`
(backend data server) and `COORDINATOR` (frontend server for external and 
application access).

For Active Failover deployments, the role needs to be `SINGLE`.)");

  options
      ->addOption("--cluster.my-address",
                  "This server's endpoint for cluster-internal communication.",
                  new StringParameter(&_myEndpoint),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator,
                      arangodb::options::Flags::OnDBServer))
      .setLongDescription(R"(If specified, the endpoint needs to be in one of
the following formats:

- `tcp://ipv4-address:port` - TCP/IP endpoint, using IPv4
- `tcp://[ipv6-address]:port` - TCP/IP endpoint, using IPv6
- `ssl://ipv4-address:port` - TCP/IP endpoint, using IPv4, SSL encryption
- `ssl://[ipv6-address]:port` - TCP/IP endpoint, using IPv6, SSL encryption

If you don't specify an endpoint, the server looks up its internal endpoint
address in the Agency. If no endpoint can be found in the Agency for the
server's ID, ArangoDB refuses to start.

**Examples**

Listen only on the interface with the address `192.168.1.1`:

`--cluster.my-address tcp://192.168.1.1:8530`

Listen on all IPv4 and IPv6 addresses which are configured on port `8530`:

`--cluster.my-address ssl://[::]:8530`)");

  options
      ->addOption("--cluster.my-advertised-endpoint",
                  "This server's advertised endpoint for external "
                  "communication (optional, e.g. aan external IP address or "
                  "load balancer).",
                  new StringParameter(&_myAdvertisedEndpoint),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator,
                      arangodb::options::Flags::OnDBServer))
      .setLongDescription(R"(If specified, the endpoint needs to be in one of
the following formats:

- `tcp://ipv4-address:port` - TCP/IP endpoint, using IPv4
- `tcp://[ipv6-address]:port` - TCP/IP endpoint, using IPv6
- `ssl://ipv4-address:port` - TCP/IP endpoint, using IPv4, SSL encryption
- `ssl://[ipv6-address]:port` - TCP/IP endpoint, using IPv6, SSL encryption

If you don't specify an advertised endpoint, no external endpoint is
advertised.

**Examples**

If an external interface is available to this server, you can specify it to
communicate with external software / drivers:

`--cluster.my-advertised-endpoint tcp://some.public.place:8530`

All specifications of endpoints apply.)");

  options
      ->addOption("--cluster.write-concern",
                  "The global default write concern used for writes to new "
                  "collections.",
                  new UInt32Parameter(&_writeConcern),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30600)
      .setLongDescription(R"(This value is used as the default write concern
for databases, which in turn is used as the default for collections.

**Warning**: If you use multiple Coordinators, use the same value on all
Coordinators.)");

  options
      ->addOption("--cluster.system-replication-factor",
                  "The default replication factor for system collections.",
                  new UInt32Parameter(&_systemReplicationFactor),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator))
      .setLongDescription(R"(**Warning**: If you use multiple Coordinators, use
the same value on all Coordinators.)");

  options
      ->addOption("--cluster.default-replication-factor",
                  "The default replication factor for non-system collections.",
                  new UInt32Parameter(&_defaultReplicationFactor),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30600)
      .setLongDescription(R"(If you don't set this option, it defaults to the
value of the `--cluster.min-replication-factor` option. If set, the value must
be between the values of `--cluster.min-replication-factor` and
`--cluster.max-replication-factor`.

Note that you can still adjust the replication factor per collection. This value
is only the default value used for new collections if no replication factor is
specified when creating a collection.

**Warning**: If you use multiple Coordinators, use the same value on all
Coordinators.)");

  options
      ->addOption("--cluster.min-replication-factor",
                  "The minimum replication factor for new collections.",
                  new UInt32Parameter(&_minReplicationFactor, /*base*/ 1,
                                      /*minValue*/ 1),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30600)
      .setLongDescription(R"(If you change the value of this setting and
restart the servers, no changes are applied to existing collections that would
violate the new setting.

**Warning**: If you use multiple Coordinators, use the same value on all
Coordinators.)");

  options
      ->addOption("--cluster.max-replication-factor",
                  "The maximum replication factor for new collections "
                  "(0 = unrestricted).",
                  // 10 is a hard-coded max value for the replication factor
                  new UInt32Parameter(&_maxReplicationFactor, /*base*/ 1,
                                      /*minValue*/ 0, /*maxValue*/ 10),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30600)
      .setLongDescription(R"(If you change the value of this setting and
restart the servers, no changes are applied to existing collections that would
violate the new setting.

**Warning**: If you use multiple Coordinators, use the same value on all
Coordinators.)");

  options
      ->addOption(
          "--cluster.max-number-of-shards",
          "The maximum number of shards that can be configured when creating "
          "new collections (0 = unrestricted).",
          new UInt32Parameter(&_maxNumberOfShards, /*base*/ 1, /*minValue*/ 1),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30501)
      .setLongDescription(R"(If you change the value of this setting and
restart the servers, no changes are applied to existing collections that would
violate the new setting.

**Warning**: If you use multiple Coordinators, use the same value on all
Coordinators.)");

  options
      ->addOption("--cluster.force-one-shard",
                  "Force the OneShard mode for all new collections.",
                  new BooleanParameter(&_forceOneShard),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator,
                      arangodb::options::Flags::Enterprise))
      .setIntroducedIn(30600)
      .setLongDescription(R"(If set to `true`, forces the cluster into creating
all future collections with only a single shard and using the same DB-Server as
as these collections' shards leader. All collections created this way are
eligible for specific AQL query optimizations that can improve query performance
and provide advanced transactional guarantees.

**Warning**: If you use multiple Coordinators, use the same value on all
Coordinators.)");

  options->addOption(
      "--cluster.create-waits-for-sync-replication",
      "Let the active Coordinator wait for all replicas to create collections.",
      new BooleanParameter(&_createWaitsForSyncReplication),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnCoordinator,
          arangodb::options::Flags::OnDBServer,
          arangodb::options::Flags::Uncommon));

  options->addOption(
      "--cluster.index-create-timeout",
      "The amount of time (in seconds) the Coordinator waits for an index to "
      "be created before giving up.",
      new DoubleParameter(&_indexCreationTimeout),
      arangodb::options::makeFlags(
          arangodb::options::Flags::DefaultNoComponents,
          arangodb::options::Flags::OnCoordinator,
          arangodb::options::Flags::Uncommon));

  options
      ->addOption(
          "--cluster.api-jwt-policy",
          "Controls the access permissions required for accessing "
          "/_admin/cluster REST APIs (jwt-all = JWT required to access all "
          "operations, jwt-write = JWT required for POST/PUT/DELETE "
          "operations, jwt-compat = 3.7 compatibility mode)",
          new DiscreteValuesParameter<StringParameter>(
              &_apiJwtPolicy,
              std::unordered_set<std::string>{"jwt-all", "jwt-write",
                                              "jwt-compat"}),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30800)
      .setLongDescription(R"(The possible values for the option are:

- `jwt-all`: requires a valid JWT for all accesses to `/_admin/cluster` and its
  sub-routes. If you use this configuration, the **CLUSTER** and **NODES**
  sections of the web interface are disabled, as they rely on the ability to
  read data from several cluster APIs.

- `jwt-write`: requires a valid JWT for write accesses (all HTTP methods except
  GET) to `/_admin/cluster`. You can use this setting to allow privileged users
  to read data from the cluster APIs, but not to do any modifications.
  Modifications (carried out by write accesses) are then only possible by
  requests with a valid JWT.

  All existing permission checks for the cluster API routes are still in effect
  with this setting, meaning that read operations without a valid JWT may still
  require dedicated other permissions (as in v3.7).

- `jwt-compat`: no **additional** access checks are in place for the cluster
  APIs. However, all existing permissions checks for the cluster API routes are
  still in effect with this setting, meaning that all operations may still
  require dedicated other permissions (as in v3.7).

The default value is `jwt-compat`, which means that this option does not cause
any extra JWT checks compared to v3.7.)");

  options
      ->addOption("--cluster.max-number-of-move-shards",
                  "The number of shards to be moved per rebalance operation. "
                  "If set to 0, no shards are moved.",
                  new UInt32Parameter(&_maxNumberOfMoveShards),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator))
      .setIntroducedIn(30900)
      .setLongDescription(R"(This option limits the maximum number of move
shards operations that can be made when the **Rebalance Shards** button is
clicked in the web interface. For backwards compatibility, the default value is
`10`. A value of `0` disables the button.)");

  options
      ->addOption(
          "--cluster.failed-write-concern-status-code",
          "The HTTP status code to send if a shard has not enough in-sync "
          "replicas to fulfill the write concern.",
          new DiscreteValuesParameter<UInt32Parameter>(
              &_statusCodeFailedWriteConcern, {403, 503}),
          arangodb::options::makeFlags(
              arangodb::options::Flags::DefaultNoComponents,
              arangodb::options::Flags::OnDBServer))
      .setIntroducedIn(31100)
      .setLongDescription(R"(The default behavior is to return an HTTP
`403 Forbidden` status code. You can set the option to `503` to return a
`503 Service Unavailable`.)");

  options
      ->addOption("--cluster.connectivity-check-interval",
                  "The interval (in seconds) in which cluster-internal "
                  "connectivity checks are performed.",
                  new UInt32Parameter(&_connectivityCheckInterval),
                  arangodb::options::makeFlags(
                      arangodb::options::Flags::DefaultNoComponents,
                      arangodb::options::Flags::OnCoordinator,
                      arangodb::options::Flags::OnDBServer))
      .setLongDescription(R"(Setting this option to a value greater than
zero makes Coordinators and DB-Servers run period connectivity checks
with approximately the specified frequency. The first connectivity check
is carried out approximately 15 seconds after server start.
Note that a random delay is added to the interval on each server, so that
different servers do not execute their connectivity checks all at the
same time.
Setting this option to a value of zero disables these connectivity checks.")")
      .setIntroducedIn(31104);
}

void ClusterFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  if (options->processingResult().touched(
          "cluster.disable-dispatcher-kickstarter") ||
      options->processingResult().touched(
          "cluster.disable-dispatcher-frontend")) {
    LOG_TOPIC("33707", FATAL, arangodb::Logger::CLUSTER)
        << "The dispatcher feature isn't available anymore. Use "
        << "ArangoDB Starter for this now! See "
        << "https://github.com/arangodb-helper/arangodb/ for more "
        << "details.";
    FATAL_ERROR_EXIT();
  }

  if (_forceOneShard) {
    _maxNumberOfShards = 1;
  }

  TRI_ASSERT(_minReplicationFactor > 0);
  if (!options->processingResult().touched(
          "cluster.default-replication-factor")) {
    // no default replication factor set. now use the minimum value, which is
    // guaranteed to be at least 1
    _defaultReplicationFactor = _minReplicationFactor;
  }

  if (!options->processingResult().touched(
          "cluster.system-replication-factor")) {
    // no system replication factor set. now make sure it is between min and max
    if (_systemReplicationFactor > _maxReplicationFactor) {
      _systemReplicationFactor = _maxReplicationFactor;
    } else if (_systemReplicationFactor < _minReplicationFactor) {
      _systemReplicationFactor = _minReplicationFactor;
    }
  }

  if (_defaultReplicationFactor == 0) {
    // default replication factor must not be 0
    LOG_TOPIC("fc8a9", FATAL, arangodb::Logger::CLUSTER)
        << "Invalid value for `--cluster.default-replication-factor`. The "
           "value must be at least 1";
    FATAL_ERROR_EXIT();
  }

  if (_systemReplicationFactor == 0) {
    // default replication factor must not be 0
    LOG_TOPIC("46935", FATAL, arangodb::Logger::CLUSTER)
        << "Invalid value for `--cluster.system-replication-factor`. The value "
           "must be at least 1";
    FATAL_ERROR_EXIT();
  }

  if (_defaultReplicationFactor > 0 && _maxReplicationFactor > 0 &&
      _defaultReplicationFactor > _maxReplicationFactor) {
    LOG_TOPIC("5af7e", FATAL, arangodb::Logger::CLUSTER)
        << "Invalid value for `--cluster.default-replication-factor`. Must not "
           "be higher than `--cluster.max-replication-factor`";
    FATAL_ERROR_EXIT();
  }

  if (_defaultReplicationFactor > 0 &&
      _defaultReplicationFactor < _minReplicationFactor) {
    LOG_TOPIC("b9aea", FATAL, arangodb::Logger::CLUSTER)
        << "Invalid value for `--cluster.default-replication-factor`. Must not "
           "be lower than `--cluster.min-replication-factor`";
    FATAL_ERROR_EXIT();
  }

  if (_systemReplicationFactor > 0 && _maxReplicationFactor > 0 &&
      _systemReplicationFactor > _maxReplicationFactor) {
    LOG_TOPIC("6cf0c", FATAL, arangodb::Logger::CLUSTER)
        << "Invalid value for `--cluster.system-replication-factor`. Must not "
           "be higher than `--cluster.max-replication-factor`";
    FATAL_ERROR_EXIT();
  }
  if (_systemReplicationFactor > 0 &&
      _systemReplicationFactor < _minReplicationFactor) {
    LOG_TOPIC("dfc38", FATAL, arangodb::Logger::CLUSTER)
        << "Invalid value for `--cluster.system-replication-factor`. Must not "
           "be lower than `--cluster.min-replication-factor`";
    FATAL_ERROR_EXIT();
  }

  // check if the cluster is enabled
  _enableCluster = !_agencyEndpoints.empty();
  if (!_enableCluster) {
    _requestedRole = ServerState::ROLE_SINGLE;
    ServerState::instance()->setRole(ServerState::ROLE_SINGLE);
    ServerState::instance()->findHost("localhost");
    return;
  }

  // validate --cluster.agency-endpoint
  if (_agencyEndpoints.empty()) {
    LOG_TOPIC("d283a", FATAL, Logger::CLUSTER)
        << "must at least specify one endpoint in --cluster.agency-endpoint";
    FATAL_ERROR_EXIT();
  }

  // validate --cluster.my-address
  if (_myEndpoint.empty()) {
    LOG_TOPIC("c1532", FATAL, arangodb::Logger::CLUSTER)
        << "unable to determine internal address for server '"
        << ServerState::instance()->getId()
        << "'. Please specify --cluster.my-address or configure the "
           "address for this server in the agency.";
    FATAL_ERROR_EXIT();
  }

  // now we can validate --cluster.my-address
  if (Endpoint::unifiedForm(_myEndpoint).empty()) {
    LOG_TOPIC("41256", FATAL, arangodb::Logger::CLUSTER)
        << "invalid endpoint '" << _myEndpoint
        << "' specified for --cluster.my-address";
    FATAL_ERROR_EXIT();
  }
  if (!_myAdvertisedEndpoint.empty() &&
      Endpoint::unifiedForm(_myAdvertisedEndpoint).empty()) {
    LOG_TOPIC("ece6a", FATAL, arangodb::Logger::CLUSTER)
        << "invalid endpoint '" << _myAdvertisedEndpoint
        << "' specified for --cluster.my-advertised-endpoint";
    FATAL_ERROR_EXIT();
  }

  // changing agency namespace no longer needed
  _agencyPrefix = "arango";

  // validate system-replication-factor
  if (_systemReplicationFactor == 0) {
    LOG_TOPIC("cb945", FATAL, arangodb::Logger::CLUSTER)
        << "system replication factor must be greater 0";
    FATAL_ERROR_EXIT();
  }

  std::string fallback = _myEndpoint;
  // Now extract the hostname/IP:
  auto pos = fallback.find("://");
  if (pos != std::string::npos) {
    fallback = fallback.substr(pos + 3);
  }
  pos = fallback.rfind(':');
  if (pos != std::string::npos) {
    fallback = fallback.substr(0, pos);
  }
  auto ss = ServerState::instance();
  ss->findHost(fallback);

  if (!_myRole.empty()) {
    _requestedRole = ServerState::stringToRole(_myRole);

    std::vector<arangodb::ServerState::RoleEnum> const disallowedRoles = {
        /*ServerState::ROLE_SINGLE,*/ ServerState::ROLE_AGENT,
        ServerState::ROLE_UNDEFINED};

    if (std::find(disallowedRoles.begin(), disallowedRoles.end(),
                  _requestedRole) != disallowedRoles.end()) {
      LOG_TOPIC("198c3", FATAL, arangodb::Logger::CLUSTER)
          << "Invalid role provided for `--cluster.my-role`. Possible values: "
             "DBSERVER, PRIMARY, COORDINATOR";
      FATAL_ERROR_EXIT();
    }
    ServerState::instance()->setRole(_requestedRole);
  }

  constexpr std::uint32_t minConnectivityCheckInterval = 10;  // seconds
  if (_connectivityCheckInterval > 0 &&
      _connectivityCheckInterval < minConnectivityCheckInterval) {
    _connectivityCheckInterval = minConnectivityCheckInterval;
    LOG_TOPIC("08b46", WARN, Logger::CLUSTER)
        << "configured value for `--cluster.connectivity-check-interval` is "
           "too low and was automatically adjusted to minimum value "
        << minConnectivityCheckInterval;
  }
}

void ClusterFeature::reportRole(arangodb::ServerState::RoleEnum role) {
  std::string roleString(ServerState::roleToString(role));
  if (role == ServerState::ROLE_UNDEFINED) {
    roleString += ". Determining real role from agency";
  }
  LOG_TOPIC("3bb7d", INFO, arangodb::Logger::CLUSTER)
      << "Starting up with role " << roleString;
}

// IMPORTANT: Please make sure that you understand that the agency is not
// available before ::start and this should not be accessed in this section.
void ClusterFeature::prepare() {
  if (_enableCluster && _requirePersistedId &&
      !ServerState::instance()->hasPersistedId()) {
    LOG_TOPIC("d2194", FATAL, arangodb::Logger::CLUSTER)
        << "required persisted UUID file '"
        << ServerState::instance()->getUuidFilename()
        << "' not found. Please make sure this instance is started using an "
           "already existing database directory";
    FATAL_ERROR_EXIT();
  }

  if (_agencyCache == nullptr || _clusterInfo == nullptr) {
    allocateMembers();
  }

  if (ServerState::instance()->isAgent() || _enableCluster) {
    AuthenticationFeature* af = AuthenticationFeature::instance();
    if (af->isActive() && !af->hasUserdefinedJwt()) {
      LOG_TOPIC("6e615", FATAL, arangodb::Logger::CLUSTER)
          << "Cluster authentication enabled but JWT not set via command line. "
             "Please provide --server.jwt-secret-keyfile or "
             "--server.jwt-secret-folder which is used throughout the cluster.";
      FATAL_ERROR_EXIT();
    }
  }

  // return if cluster is disabled
  if (!_enableCluster) {
    reportRole(ServerState::instance()->getRole());
    return;
  }

  reportRole(_requestedRole);

  network::ConnectionPool::Config config(_metrics);
  config.numIOThreads = 2u;
  config.maxOpenConnections = 2;
  config.idleConnectionMilli = 10000;
  config.verifyHosts = false;
  config.clusterInfo = &clusterInfo();
  config.name = "AgencyComm";

  _asyncAgencyCommPool = std::make_unique<network::ConnectionPool>(config);

  // register the prefix with the communicator
  AgencyCommHelper::initialize(_agencyPrefix);
  AsyncAgencyCommManager::initialize(server());
  TRI_ASSERT(AsyncAgencyCommManager::INSTANCE != nullptr);
  AsyncAgencyCommManager::INSTANCE->setSkipScheduler(true);
  AsyncAgencyCommManager::INSTANCE->pool(_asyncAgencyCommPool.get());

  for (const auto& _agencyEndpoint : _agencyEndpoints) {
    std::string const unified = Endpoint::unifiedForm(_agencyEndpoint);

    if (unified.empty()) {
      LOG_TOPIC("1b759", FATAL, arangodb::Logger::CLUSTER)
          << "invalid endpoint '" << _agencyEndpoint
          << "' specified for --cluster.agency-endpoint";
      FATAL_ERROR_EXIT();
    }

    AsyncAgencyCommManager::INSTANCE->addEndpoint(unified);
  }

  bool ok = AgencyComm(server()).ensureStructureInitialized();
  LOG_TOPIC("d8ce6", DEBUG, Logger::AGENCYCOMM)
      << "structures " << (ok ? "are" : "failed to") << " initialize";

  if (!ok) {
    LOG_TOPIC("54560", FATAL, arangodb::Logger::CLUSTER)
        << "Could not connect to any agency endpoints ("
        << AsyncAgencyCommManager::INSTANCE->endpointsString() << ")";
    FATAL_ERROR_EXIT();
  }

  // This must remain here for proper function after hot restores
  auto role = ServerState::instance()->getRole();
  if (role != ServerState::ROLE_AGENT && role != ServerState::ROLE_UNDEFINED) {
    if (!_agencyCache->start()) {
      LOG_TOPIC("bae31", DEBUG, Logger::CLUSTER)
          << "Waiting for agency cache to become ready.";
      FATAL_ERROR_EXIT();
    }
  }

  if (!ServerState::instance()->integrateIntoCluster(
          _requestedRole, _myEndpoint, _myAdvertisedEndpoint)) {
    LOG_TOPIC("fea1e", FATAL, Logger::STARTUP)
        << "Couldn't integrate into cluster.";
    FATAL_ERROR_EXIT();
  }

  auto endpoints = AsyncAgencyCommManager::INSTANCE->endpoints();

  if (role == ServerState::ROLE_UNDEFINED) {
    // no role found
    LOG_TOPIC("613f4", FATAL, arangodb::Logger::CLUSTER)
        << "unable to determine unambiguous role for server '"
        << ServerState::instance()->getId()
        << "'. No role configured in agency (" << endpoints << ")";
    FATAL_ERROR_EXIT();
  }
}

DECLARE_COUNTER(arangodb_dropped_followers_total,
                "Number of drop-follower events");
DECLARE_COUNTER(
    arangodb_refused_followers_total,
    "Number of refusal answers from a follower during synchronous replication");
DECLARE_COUNTER(arangodb_sync_wrong_checksum_total,
                "Number of times a mismatching shard checksum was detected "
                "when syncing shards");
DECLARE_COUNTER(arangodb_sync_rebuilds_total,
                "Number of times a follower shard needed to be completely "
                "rebuilt because of too many synchronization failures");
DECLARE_COUNTER(arangodb_sync_tree_rebuilds_total,
                "Number of times a shard rebuilt its revision tree "
                "completely because of too many synchronization failures");
DECLARE_COUNTER(arangodb_potentially_dirty_document_reads_total,
                "Number of document reads which could be dirty");
DECLARE_COUNTER(arangodb_dirty_read_queries_total,
                "Number of queries which could be doing dirty reads");
DECLARE_COUNTER(arangodb_network_connectivity_failures_coordinators_total,
                "Number of times the cluster-internal connectivity check for "
                "Coordinators failed.");
DECLARE_COUNTER(arangodb_network_connectivity_failures_dbservers_total,
                "Number of times the cluster-internal connectivity check for "
                "DB-Servers failed.");

// IMPORTANT: Please read the first comment block a couple of lines down, before
// Adding code to this section.
void ClusterFeature::start() {
  // return if cluster is disabled
  if (!_enableCluster) {
    startHeartbeatThread(nullptr, 5000, 5, std::string());
    return;
  }

  auto role = ServerState::instance()->getRole();

  // We need to wait for any cluster operation, which needs access to the
  // agency cache for it to become ready. The essentials in the cluster, namely
  // ClusterInfo etc, need to start after first poll result from the agency.
  // This is of great importance to not accidentally delete data facing an
  // empty agency. There are also other measures that guard against such a
  // outcome. But there is also no point continuing with a first agency poll.
  if (role != ServerState::ROLE_AGENT && role != ServerState::ROLE_UNDEFINED) {
    _agencyCache->waitFor(1).get();
    LOG_TOPIC("13eab", DEBUG, Logger::CLUSTER)
        << "Agency cache is ready. Starting cluster cache syncers";
  }

  // If we are a coordinator, we wait until at least one DBServer is there,
  // otherwise we can do very little, in particular, we cannot create
  // any collection:
  if (role == ServerState::ROLE_COORDINATOR) {
    double start = TRI_microtime();
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // in maintainer mode, a developer does not want to spend that much time
    // waiting for extra nodes to start up
    constexpr double waitTime = 5.0;
#else
    constexpr double waitTime = 15.0;
#endif
    while (true) {
      LOG_TOPIC("d4db4", INFO, arangodb::Logger::CLUSTER)
          << "Waiting for DBservers to show up...";

      _clusterInfo->loadCurrentDBServers();
      std::vector<ServerID> DBServers = _clusterInfo->getCurrentDBServers();
      if (DBServers.size() >= 1 &&
          (DBServers.size() > 1 || TRI_microtime() - start > waitTime)) {
        LOG_TOPIC("22f55", INFO, arangodb::Logger::CLUSTER)
            << "Found " << DBServers.size() << " DBservers.";
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  ServerState::instance()->setState(ServerState::STATE_STARTUP);

  // tell the agency about our state
  AgencyComm comm(server());
  comm.sendServerState(120.0);

  auto const version = comm.version();

  ServerState::instance()->setInitialized();

  std::string const endpoints =
      AsyncAgencyCommManager::INSTANCE->getCurrentEndpoint();

  std::string myId = ServerState::instance()->getId();

  if (role == ServerState::RoleEnum::ROLE_DBSERVER) {
    _followersDroppedCounter =
        &_metrics.add(arangodb_dropped_followers_total{});
    _followersRefusedCounter =
        &_metrics.add(arangodb_refused_followers_total{});
    _followersWrongChecksumCounter =
        &_metrics.add(arangodb_sync_wrong_checksum_total{});
    _followersTotalRebuildCounter =
        &_metrics.add(arangodb_sync_rebuilds_total{});
    _syncTreeRebuildCounter =
        &_metrics.add(arangodb_sync_tree_rebuilds_total{});
  } else if (role == ServerState::RoleEnum::ROLE_COORDINATOR) {
    _potentiallyDirtyDocumentReadsCounter =
        &_metrics.add(arangodb_potentially_dirty_document_reads_total{});
    _dirtyReadQueriesCounter =
        &_metrics.add(arangodb_dirty_read_queries_total{});
  }

  if (role == ServerState::RoleEnum::ROLE_DBSERVER ||
      role == ServerState::RoleEnum::ROLE_COORDINATOR) {
    _connectivityCheckFailsCoordinators = &_metrics.add(
        arangodb_network_connectivity_failures_coordinators_total{});
    _connectivityCheckFailsDBServers =
        &_metrics.add(arangodb_network_connectivity_failures_dbservers_total{});
  }

  LOG_TOPIC("b6826", INFO, arangodb::Logger::CLUSTER)
      << "Cluster feature is turned on"
      << (_forceOneShard ? " with one-shard mode" : "")
      << ". Agency version: " << version << ", Agency endpoints: " << endpoints
      << ", server id: '" << myId
      << "', internal endpoint / address: " << _myEndpoint
      << ", advertised endpoint: "
      << (_myAdvertisedEndpoint.empty() ? "-" : _myAdvertisedEndpoint)
      << ", role: " << ServerState::roleToString(role);

  auto [acb, idx] = _agencyCache->read(std::vector<std::string>{
      AgencyCommHelper::path("Sync/HeartbeatIntervalMs")});
  auto result = acb->slice();

  if (result.isArray()) {
    velocypack::Slice HeartbeatIntervalMs =
        result[0].get(std::vector<std::string>(
            {AgencyCommHelper::path(), "Sync", "HeartbeatIntervalMs"}));

    if (HeartbeatIntervalMs.isInteger()) {
      try {
        _heartbeatInterval = HeartbeatIntervalMs.getUInt();
        LOG_TOPIC("805b2", INFO, arangodb::Logger::CLUSTER)
            << "using heartbeat interval value '" << _heartbeatInterval
            << " ms' from agency";
      } catch (...) {
        // Ignore if it is not a small int or uint
      }
    }
  }

  // no value set in agency. use default
  if (_heartbeatInterval == 0) {
    _heartbeatInterval = 5000;  // 1/s
    LOG_TOPIC("3d871", WARN, arangodb::Logger::CLUSTER)
        << "unable to read heartbeat interval from agency. Using "
        << "default value '" << _heartbeatInterval << " ms'";
  }

  startHeartbeatThread(_agencyCallbackRegistry.get(), _heartbeatInterval, 5,
                       endpoints);
  _clusterInfo->startSyncers();

  comm.increment("Current/Version");

  AsyncAgencyCommManager::INSTANCE->setSkipScheduler(false);
  ServerState::instance()->setState(ServerState::STATE_SERVING);

#ifdef USE_ENTERPRISE
  // If we are on a coordinator, we want to have a callback which is called
  // whenever a hotbackup restore is done:
  if (role == ServerState::ROLE_COORDINATOR) {
    auto hotBackupRestoreDone = [this](VPackSlice const& result) -> bool {
      if (!server().isStopping()) {
        LOG_TOPIC("12636", INFO, Logger::BACKUP)
            << "Got a hotbackup restore "
               "event, getting new cluster-wide unique IDs...";
        this->_clusterInfo->uniqid(1000000);
      }
      return true;
    };
    _hotbackupRestoreCallback =
        std::make_shared<AgencyCallback>(server(), "Sync/HotBackupRestoreDone",
                                         hotBackupRestoreDone, true, false);
    Result r = _agencyCallbackRegistry->registerCallback(
        _hotbackupRestoreCallback, true);
    if (r.fail()) {
      LOG_TOPIC("82516", WARN, Logger::BACKUP)
          << "Could not register hotbackup restore callback, this could lead "
             "to problems after a restore!";
    }
  }
#endif

  if (_connectivityCheckInterval > 0 &&
      (role == ServerState::ROLE_COORDINATOR ||
       role == ServerState::ROLE_DBSERVER)) {
    // if connectivity checks are enabled, start the first one 15s after
    // ClusterFeature start. we also add a bit of random noise to the start
    // time offset so that when multiple servers are started at the same time,
    // they don't execute their connectivity checks all at the same time
    scheduleConnectivityCheck(15 +
                              RandomGenerator::interval(std::uint32_t(15)));
  }
}

void ClusterFeature::beginShutdown() {
  if (_enableCluster) {
    _clusterInfo->shutdownSyncers();

    std::lock_guard<std::mutex> guard(_connectivityCheckMutex);
    _connectivityCheck.reset();
  }
  _agencyCache->beginShutdown();
}

void ClusterFeature::stop() {
  shutdownHeartbeatThread();

  if (_enableCluster) {
    {
      std::lock_guard<std::mutex> guard(_connectivityCheckMutex);
      _connectivityCheck.reset();
    }

#ifdef USE_ENTERPRISE
    if (_hotbackupRestoreCallback != nullptr) {
      if (!_agencyCallbackRegistry->unregisterCallback(
              _hotbackupRestoreCallback)) {
        LOG_TOPIC("84152", DEBUG, Logger::BACKUP)
            << "Strange, we could not "
               "unregister the hotbackup restore callback.";
      }
    }
#endif

    // change into shutdown state
    ServerState::instance()->setState(ServerState::STATE_SHUTDOWN);

    // wait only a few seconds to broadcast our "shut down" state.
    // if we wait much longer, and the agency has already been shut
    // down, we may cause our instance to hopelessly hang and try
    // to write something into a non-existing agency.
    AgencyComm comm(server());
    // this will be stored in transient only
    comm.sendServerState(4.0);

    // the following ops will be stored in Plan/Current (for unregister) or
    // Current (for logoff)
    if (_unregisterOnShutdown) {
      // also use a relatively short timeout here, for the same reason as above.
      ServerState::instance()->unregister(30.0);
    } else {
      // log off the server from the agency, without permanently removing it
      // from the cluster setup.
      ServerState::instance()->logoff(10.0);
    }

    AsyncAgencyCommManager::INSTANCE->setStopping(true);

    shutdown();

    // We try to actively cancel all open requests that may still be in the
    // Agency We cannot react to them anymore.
    _asyncAgencyCommPool->shutdownConnections();
    _asyncAgencyCommPool->drainConnections();
    _asyncAgencyCommPool->stop();
  }
}

void ClusterFeature::unprepare() {
  if (!_enableCluster) {
    return;
  }
  _clusterInfo->cleanup();
}

void ClusterFeature::shutdown() try {
  if (!_enableCluster) {
    shutdownHeartbeatThread();
  }

  if (_clusterInfo != nullptr) {
    _clusterInfo->shutdownSyncers();
  }

  // force shutdown of AgencyCache. under normal circumstances the cache will
  // have been shut down already when we get here, but there are rare cases in
  // which ClusterFeature::stop() isn't called (e.g. during testing or if
  // something goes very wrong at startup)
  shutdownAgencyCache();

  // force shutdown of Plan/Current syncers. under normal circumstances they
  // have been shut down already when we get here, but there are rare cases in
  // which ClusterFeature::stop() isn't called (e.g. during testing or if
  // something goes very wrong at startup)
  waitForSyncersToStop();

  // make sure agency cache is unreachable now
  _agencyCache.reset();

  // must make sure that the HeartbeatThread is fully stopped before
  // we destroy the AgencyCallbackRegistry.
  _heartbeatThread.reset();

  if (_asyncAgencyCommPool) {
    _asyncAgencyCommPool->drainConnections();
    _asyncAgencyCommPool->stop();
  }
} catch (...) {
  // this is called from the dtor. not much we can do here except logging
  LOG_TOPIC("9f538", WARN, Logger::CLUSTER)
      << "caught exception during cluster shutdown";
}

void ClusterFeature::setUnregisterOnShutdown(bool unregisterOnShutdown) {
  _unregisterOnShutdown = unregisterOnShutdown;
}

/// @brief common routine to start heartbeat with or without cluster active
void ClusterFeature::startHeartbeatThread(
    AgencyCallbackRegistry* agencyCallbackRegistry, uint64_t interval_ms,
    uint64_t maxFailsBeforeWarning, std::string const& endpoints) {
  _heartbeatThread = std::make_shared<HeartbeatThread>(
      server(), agencyCallbackRegistry,
      std::chrono::microseconds(interval_ms * 1000), maxFailsBeforeWarning);

  if (!_heartbeatThread->init() || !_heartbeatThread->start()) {
    // failure only occures in cluster mode.
    LOG_TOPIC("7e050", FATAL, arangodb::Logger::CLUSTER)
        << "heartbeat could not connect to agency endpoints (" << endpoints
        << ")";
    FATAL_ERROR_EXIT();
  }

  while (!_heartbeatThread->isReady()) {
    // wait until heartbeat is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void ClusterFeature::pruneAsyncAgencyConnectionPool() {
  _asyncAgencyCommPool->pruneConnections();
}

void ClusterFeature::shutdownHeartbeatThread() {
  if (_heartbeatThread != nullptr) {
    _heartbeatThread->beginShutdown();
    auto start = std::chrono::steady_clock::now();
    size_t counter = 0;
    while (_heartbeatThread->isRunning()) {
      if (std::chrono::steady_clock::now() - start > std::chrono::seconds(65)) {
        LOG_TOPIC("d8a5b", FATAL, Logger::CLUSTER)
            << "exiting prematurely as we failed terminating the heartbeat "
               "thread";
        FATAL_ERROR_EXIT();
      }
      if (++counter % 50 == 0) {
        LOG_TOPIC("acaa9", WARN, arangodb::Logger::CLUSTER)
            << "waiting for heartbeat thread to finish";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

/// @brief wait for the Plan and Current syncer to shut down
/// note: this may be called multiple times during shutdown
void ClusterFeature::waitForSyncersToStop() {
  if (_clusterInfo != nullptr) {
    _clusterInfo->waitForSyncersToStop();
  }
}

/// @brief wait for the AgencyCache to shut down
/// note: this may be called multiple times during shutdown
void ClusterFeature::shutdownAgencyCache() {
  if (_agencyCache != nullptr) {
    _agencyCache->beginShutdown();
    auto start = std::chrono::steady_clock::now();
    size_t counter = 0;
    while (_agencyCache != nullptr && _agencyCache->isRunning()) {
      if (std::chrono::steady_clock::now() - start > std::chrono::seconds(65)) {
        LOG_TOPIC("b5a8d", FATAL, Logger::CLUSTER)
            << "exiting prematurely as we failed terminating the agency cache";
        FATAL_ERROR_EXIT();
      }
      if (++counter % 50 == 0) {
        LOG_TOPIC("acab0", WARN, arangodb::Logger::CLUSTER)
            << "waiting for agency cache thread to finish";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void ClusterFeature::notify() {
  if (_heartbeatThread != nullptr) {
    _heartbeatThread->notify();
  }
}

std::shared_ptr<HeartbeatThread> ClusterFeature::heartbeatThread() {
  return _heartbeatThread;
}

ClusterInfo& ClusterFeature::clusterInfo() {
  if (!_clusterInfo) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  return *_clusterInfo;
}

AgencyCache& ClusterFeature::agencyCache() {
  if (_agencyCache == nullptr) {  // previous read
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  return *_agencyCache;
}

void ClusterFeature::allocateMembers() {
  _agencyCallbackRegistry =
      std::make_unique<AgencyCallbackRegistry>(server(), agencyCallbacksPath());
  _clusterInfo = std::make_unique<ClusterInfo>(
      server(), _agencyCallbackRegistry.get(), _syncerShutdownCode);
  _agencyCache = std::make_unique<AgencyCache>(
      server(), *_agencyCallbackRegistry, _syncerShutdownCode);
}

void ClusterFeature::addDirty(
    containers::FlatHashSet<std::string> const& databases, bool callNotify) {
  if (databases.size() > 0) {
    std::lock_guard guard{_dirtyLock};
    for (auto const& database : databases) {
      if (_dirtyDatabases.emplace(database).second) {
        LOG_TOPIC("35b75", DEBUG, Logger::MAINTENANCE)
            << "adding " << database << " to dirty databases";
      }
    }
    if (callNotify) {
      notify();
    }
  }
}

void ClusterFeature::addDirty(
    containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
        databases) {
  if (databases.size() > 0) {
    std::lock_guard guard{_dirtyLock};
    bool addedAny = false;
    for (auto const& database : databases) {
      if (_dirtyDatabases.emplace(database.first).second) {
        addedAny = true;
        LOG_TOPIC("35b77", DEBUG, Logger::MAINTENANCE)
            << "adding " << database << " to dirty databases";
      }
    }
    if (addedAny) {
      notify();
    }
  }
}

void ClusterFeature::addDirty(std::string const& database) {
  std::lock_guard guard{_dirtyLock};
  if (_dirtyDatabases.emplace(database).second) {
    LOG_TOPIC("357b9", DEBUG, Logger::MAINTENANCE)
        << "adding " << database << " to dirty databases";
  }
  // This notify is needed even if no database is added
  notify();
}

containers::FlatHashSet<std::string> ClusterFeature::dirty() {
  std::lock_guard guard{_dirtyLock};
  containers::FlatHashSet<std::string> ret;
  ret.swap(_dirtyDatabases);
  return ret;
}

bool ClusterFeature::isDirty(std::string const& dbName) const {
  std::lock_guard guard{_dirtyLock};
  return _dirtyDatabases.find(dbName) != _dirtyDatabases.end();
}

std::unordered_set<std::string> ClusterFeature::allDatabases() const {
  std::unordered_set<std::string> allDBNames;
  auto const tmp = server().getFeature<DatabaseFeature>().getDatabaseNames();
  allDBNames.reserve(tmp.size());
  for (auto const& i : tmp) {
    allDBNames.emplace(i);
  }
  return allDBNames;
}

void ClusterFeature::scheduleConnectivityCheck(std::uint32_t inSeconds) {
  TRI_ASSERT(_connectivityCheckInterval > 0);

  Scheduler* scheduler = SchedulerFeature::SCHEDULER;
  if (scheduler == nullptr || inSeconds == 0) {
    return;
  }

  auto workItem = arangodb::SchedulerFeature::SCHEDULER->queueDelayed(
      "connectivity-check", RequestLane::INTERNAL_LOW,
      std::chrono::seconds(inSeconds), [this](bool canceled) {
        if (canceled) {
          return;
        }

        if (!this->server().isStopping()) {
          runConnectivityCheck();
        }
        if (!this->server().isStopping()) {
          scheduleConnectivityCheck(
              _connectivityCheckInterval +
              RandomGenerator::interval(std::uint32_t(3)));
        }
      });

  std::lock_guard<std::mutex> guard(_connectivityCheckMutex);
  _connectivityCheck = std::move(workItem);
}

void ClusterFeature::runConnectivityCheck() {
  TRI_ASSERT(ServerState::instance()->isCoordinator() ||
             ServerState::instance()->isDBServer());

  TRI_ASSERT(_connectivityCheckFailsCoordinators != nullptr);
  TRI_ASSERT(_connectivityCheckFailsDBServers != nullptr);

  NetworkFeature const& nf = server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (!pool) {
    return;
  }

  if (_clusterInfo == nullptr) {
    return;
  }

  // we want to contact coordinators and DB servers, potentially
  // including _ourselves_ (we need to be able to send requests
  // to ourselves)
  auto servers = _clusterInfo->getCurrentCoordinators();
  for (auto& it : _clusterInfo->getCurrentDBServers()) {
    servers.emplace_back(std::move(it));
  }

  LOG_TOPIC("601e3", DEBUG, Logger::CLUSTER)
      << "sending connectivity check requests to " << servers.size()
      << " servers: " << servers;

  // run a basic connectivity check by calling /_api/version
  static constexpr double timeout = 10.0;
  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;
  reqOpts.timeout = network::Timeout(timeout);

  std::vector<futures::Future<network::Response>> futures;
  futures.reserve(servers.size());

  for (auto const& server : servers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + server,
                                              fuerte::RestVerb::Get,
                                              "/_api/version", {}, reqOpts));
  }

  for (futures::Future<network::Response>& f : futures) {
    if (this->server().isStopping()) {
      break;
    }
    network::Response const& r = f.get();
    TRI_ASSERT(r.destination.starts_with("server:"));

    if (r.ok()) {
      LOG_TOPIC("803c0", DEBUG, Logger::CLUSTER)
          << "connectivity check for endpoint " << r.destination
          << " successful";
    } else {
      LOG_TOPIC("43fc0", WARN, Logger::CLUSTER)
          << "unable to connect to endpoint " << r.destination << " within "
          << timeout << " seconds: " << r.combinedResult().errorMessage();

      auto ep = std::string_view(r.destination);
      if (!ep.starts_with("server:")) {
        TRI_ASSERT(false);
        continue;
      }
      // strip "server:" prefix
      ep = ep.substr(strlen("server:"));
      if (ep.starts_with("PRMR-")) {
        // DB-Server
        _connectivityCheckFailsDBServers->count();
      } else if (ep.starts_with("CRDN-")) {
        _connectivityCheckFailsCoordinators->count();
      } else {
        // unknown server type!
        TRI_ASSERT(false);
      }
    }
  }
}
