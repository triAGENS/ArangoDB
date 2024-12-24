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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "Servers.h"

#include <algorithm>

#include "Agency/AgencyStrings.h"
#include "Agency/AsyncAgencyComm.h"
#include "ApplicationFeatures/ApplicationFeature.h"
#include "ApplicationFeatures/CommunicationFeaturePhase.h"
#include "ApplicationFeatures/GreetingsFeaturePhase.h"
#include "ApplicationFeatures/HttpEndpointProvider.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/ProfileLevel.h"
#include "Aql/Query.h"
#include "Aql/QueryInfoLoggerFeature.h"
#include "Async/async.h"
#include "Basics/StringUtils.h"
#include "Basics/TimeString.h"
#include "Basics/files.h"
#include "Cluster/ActionDescription.h"
#include "Cluster/AgencyCache.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/CreateCollection.h"
#include "Cluster/CreateDatabase.h"
#include "Cluster/DropDatabase.h"
#include "Cluster/Maintenance.h"
#include "ClusterEngine/ClusterEngine.h"
#include "FeaturePhases/AqlFeaturePhase.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "FeaturePhases/ClusterFeaturePhase.h"
#include "FeaturePhases/DatabaseFeaturePhase.h"
#include "RestServer/VectorIndexFeature.h"
#ifdef USE_V8
#include "FeaturePhases/V8FeaturePhase.h"
#endif
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/ServerSecurityFeature.h"
#include "IResearch/AgencyMock.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLinkCoordinator.h"
#include "IResearch/common.h"
#include "Logger/LogMacros.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "Metrics/ClusterMetricsFeature.h"
#include "Metrics/MetricsFeature.h"
#include "Mocks/PreparedResponseConnectionPool.h"
#include "Mocks/StorageEngineMock.h"
#include "Network/NetworkFeature.h"
#include "Replication/ReplicationFeature.h"
#include "Replication2/ReplicatedLog/ReplicatedLogFeature.h"
#include "Replication2/ReplicatedState/ReplicatedStateFeature.h"
#include "Rest/Version.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/InitDatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SharedPRNGFeature.h"
#include "RestServer/SoftShutdownFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/TemporaryStorageFeature.h"
#include "RestServer/UpgradeFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "Scheduler/SchedulerFeature.h"
#include "Servers.h"
#include "Sharding/ShardingFeature.h"
#include "Statistics/StatisticsFeature.h"
#include "Statistics/StatisticsWorker.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngineFeature.h"
#include "TemplateSpecializer.h"
#include "Transaction/ManagerFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#ifdef USE_V8
#include "V8/V8SecurityFeature.h"
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/LogicalCollection.h"
#include "VocBase/vocbase.h"
#include "utils/log.hpp"

#if USE_ENTERPRISE
#include "Enterprise/Encryption/EncryptionFeature.h"
#include "Enterprise/License/LicenseFeature.h"
#include "Enterprise/StorageEngine/HotBackupFeature.h"
#endif

#include <velocypack/Builder.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>

#include <boost/core/demangle.hpp>

using namespace arangodb;
using namespace arangodb::consensus;
using namespace arangodb::tests;
using namespace arangodb::tests::mocks;

struct HttpEndpointProviderMock final : public HttpEndpointProvider {
  static constexpr std::string_view name() {
    return "HttpEndpointProviderMock";
  }

  explicit HttpEndpointProviderMock(ArangodServer& server)
      : HttpEndpointProvider{server, *this} {}

  virtual std::vector<std::string> httpEndpoints() final { return {}; }
};

static void SetupGreetingsPhase(MockServer& server) {
  server.addFeature<GreetingsFeaturePhase>(false, std::false_type{});
  server.addFeature<metrics::MetricsFeature>(
      false, LazyApplicationFeatureReference<QueryRegistryFeature>(nullptr),
      LazyApplicationFeatureReference<StatisticsFeature>(nullptr),
      LazyApplicationFeatureReference<EngineSelectorFeature>(nullptr),
      LazyApplicationFeatureReference<metrics::ClusterMetricsFeature>(nullptr),
      LazyApplicationFeatureReference<ClusterFeature>(nullptr));
  server.addFeature<SharedPRNGFeature>(false);
  server.addFeature<SoftShutdownFeature>(false);
  // We do not need any further features from this phase
}

static void SetupBasicFeaturePhase(MockServer& server) {
  SetupGreetingsPhase(server);
  server.addFeature<BasicFeaturePhaseServer>(false);
  server.addFeature<ShardingFeature>(false);
  server.addFeature<DatabasePathFeature>(false);
}

static void SetupDatabaseFeaturePhase(MockServer& server) {
  SetupBasicFeaturePhase(server);
  server.addFeature<DatabaseFeaturePhase>(false);  // true ??
  server.addFeature<AuthenticationFeature>(true);
  server.addFeature<transaction::ManagerFeature>(false);
  server.addFeature<DatabaseFeature>(false);
  server.addFeature<EngineSelectorFeature>(false);
  server.addFeature<StorageEngineFeature>(false);
  server.addFeature<SystemDatabaseFeature>(true);
  server.addFeature<InitDatabaseFeature>(true, std::vector<size_t>{});
  server.addFeature<ViewTypesFeature>(false);  // true ??
  server.addFeature<VectorIndexFeature>(true);

#if USE_ENTERPRISE
  // required for AuthenticationFeature with USE_ENTERPRISE
  server.addFeature<LicenseFeature>(false);
  server.addFeature<EncryptionFeature>(false);
#endif
}

static void SetupClusterFeaturePhase(MockServer& server) {
  SetupDatabaseFeaturePhase(server);
  server.addFeature<ClusterFeaturePhase>(false);
  server.addFeature<ClusterFeature>(false);
  // set default replication factor to 1 for tests. otherwise the default value
  // is 0, which will lead to follow up errors if it is not corrected later.
  server.getFeature<ClusterFeature>().defaultReplicationFactor(1);

  // fake the exit code with which unresolved futures are returned on
  // shutdown. if we don't do this lots of places in ClusterInfo will
  // report failures during testing
  server.getFeature<ClusterFeature>().setSyncerShutdownCode(TRI_ERROR_NO_ERROR);
}

static void SetupCommunicationFeaturePhase(MockServer& server) {
  SetupClusterFeaturePhase(server);
  server.addFeature<HttpEndpointProvider, HttpEndpointProviderMock>(false);
  server.addFeature<CommunicationFeaturePhase>(false);
  // This phase is empty...
}

static void SetupV8Phase(MockServer& server) {
  SetupCommunicationFeaturePhase(server);
#ifdef USE_V8
  server.addFeature<V8FeaturePhase>(false);
  server.addFeature<V8DealerFeature>(
      false, server.template getFeature<arangodb::metrics::MetricsFeature>());
  server.addFeature<V8SecurityFeature>(false);
#endif
}

static void SetupAqlPhase(MockServer& server) {
  SetupV8Phase(server);
  server.addFeature<AqlFeaturePhase>(false);
  server.addFeature<QueryRegistryFeature>(
      false, server.template getFeature<arangodb::metrics::MetricsFeature>());
  server.addFeature<TemporaryStorageFeature>(false);

  server.addFeature<arangodb::iresearch::IResearchAnalyzerFeature>(true);
  {
    auto& feature =
        server.addFeature<arangodb::iresearch::IResearchFeature>(true);
    feature.collectOptions(server.server().options());
    feature.validateOptions(server.server().options());
  }

  server.addFeature<aql::AqlFunctionFeature>(true);
  server.addFeature<aql::OptimizerRulesFeature>(true);
  server.addFeature<aql::QueryInfoLoggerFeature>(true);
  server.addFeature<AqlFeature>(true);

#ifdef USE_ENTERPRISE
  server.addFeature<HotBackupFeature>(false);
#endif
}

MockServer::MockServer(ServerState::RoleEnum myRole, bool injectClusterIndexes)
    : _server(std::make_shared<options::ProgramOptions>("", "", "", nullptr),
              nullptr),
      _engine(
          std::make_unique<StorageEngineMock>(_server, injectClusterIndexes)),
      _oldRebootId(0),
      _started(false) {
  _oldRole = ServerState::instance()->getRole();
  ServerState::instance()->setRole(myRole);
  _originalMockingState = ClusterEngine::Mocking;
  if (injectClusterIndexes && ServerState::instance()->isCoordinator()) {
    ClusterEngine::Mocking = true;
  }
  init();
}

MockServer::~MockServer() {
  stopFeatures();
  _server.setStateUnsafe(_oldApplicationServerState);

  ClusterEngine::Mocking = _originalMockingState;
  ServerState::instance()->setRole(_oldRole);
  ServerState::instance()->setRebootId(_oldRebootId);
}

ArangodServer& MockServer::server() { return _server; }

void MockServer::init() {
  _oldApplicationServerState = _server.state();
  _oldRebootId = ServerState::instance()->getRebootId();

  _server.setStateUnsafe(ApplicationServer::State::IN_WAIT);
  transaction::Methods::clearDataSourceRegistrationCallbacks();

  // many other places rely on the reboot id being initialized,
  // so we do it here in a central place
  ServerState::instance()->setRebootId(RebootId{1});
}

void MockServer::startFeatures() {
  using application_features::ApplicationFeature;

  // user can no longer add features with addFeature, must add them directly
  // to underlying server()
  _started = true;

  _server.setupDependencies(false);
  auto orderedFeatures = _server.getOrderedFeatures();

  _server.getFeature<EngineSelectorFeature>().setEngineTesting(_engine.get());

  if (_server.hasFeature<SchedulerFeature>()) {
    auto& sched = _server.getFeature<SchedulerFeature>();
    // Needed to set nrMaximalThreads
    sched.validateOptions(
        std::make_shared<options::ProgramOptions>("", "", "", nullptr));
  }

  for (ApplicationFeature& f : orderedFeatures) {
    auto info = _features.find(&f);
    if (info != _features.end()) {
      if (f.name() == "Endpoint") {
        // We need this feature to be there but do not use it.
        continue;
      }
      try {
        f.prepare();
      } catch (...) {
        LOG_DEVEL << "unexpected exception in "
                  << boost::core::demangle(typeid(f).name()) << "::prepare";
      }
    }
  }

  if (_server.hasFeature<DatabaseFeature>()) {
    auto& dbFeature = _server.getFeature<DatabaseFeature>();
    // Only add a database if we have the feature.
    auto const databases = velocypack::Parser::fromJson(
        R"([{"name": ")" + StaticStrings::SystemDatabase + R"("}])");
    dbFeature.loadDatabases(databases->slice());
  }

  for (ApplicationFeature& f : orderedFeatures) {
    auto info = _features.find(&f);
    // We only start those features...
    if (info != _features.end()) {
      if (info->second) {
        try {
          f.start();
        } catch (...) {
          LOG_DEVEL << "unexpected exception in "
                    << boost::core::demangle(typeid(f).name()) << "::start";
        }
      }
    }
  }

  if (_server.hasFeature<DatabasePathFeature>()) {
    auto& dbPathFeature = _server.getFeature<DatabasePathFeature>();
    // Inject a testFileSystemPath
    tests::setDatabasePath(
        dbPathFeature);  // ensure test data is stored in a unique directory
    _testFilesystemPath = dbPathFeature.directory();

    long systemError;
    std::string systemErrorStr;
    TRI_CreateDirectory(_testFilesystemPath.c_str(), systemError,
                        systemErrorStr);
  }
}

auto MockClusterServer::genUniqId() const -> std::uint64_t {
  // We must use a consistent unique ID generation. Sadly, several IResearch
  // tests have hard-coded IDs which are expected to be generated by
  // TRI_NewTickServer().
  return TRI_NewTickServer();
}

void MockServer::stopFeatures() {
  using application_features::ApplicationFeature;

  if (!_testFilesystemPath.empty()) {
    TRI_RemoveDirectory(_testFilesystemPath.c_str());
  }

  // need to shut down in reverse order
  auto orderedFeatures = _server.getOrderedFeatures();
  std::reverse(orderedFeatures.begin(), orderedFeatures.end());

  // destroy application features
  for (ApplicationFeature& f : orderedFeatures) {
    auto info = _features.find(&f);
    // We only start those features...
    if (info != _features.end()) {
      if (info->second) {
        try {
          f.stop();
        } catch (...) {
          LOG_DEVEL << "unexpected exception in "
                    << boost::core::demangle(typeid(f).name()) << "::stop";
        }
      }
    }
  }

  for (ApplicationFeature& f : orderedFeatures) {
    auto info = _features.find(&f);
    // We only start those features...
    if (info != _features.end()) {
      try {
        f.unprepare();
      } catch (...) {
        LOG_DEVEL << "unexpected exception in "
                  << boost::core::demangle(typeid(f).name()) << "::unprepare";
      }
    }
  }
}

TRI_vocbase_t& MockServer::getSystemDatabase() const {
  TRI_ASSERT(_server.hasFeature<DatabaseFeature>());
  auto& database = _server.getFeature<DatabaseFeature>();
  auto system = database.lookupDatabase(StaticStrings::SystemDatabase);
  TRI_ASSERT(system != nullptr);
  return *system;
}

MockMetricsServer::MockMetricsServer(bool start) : MockServer() {
  // setup required application features
  SetupGreetingsPhase(*this);
  addFeature<EngineSelectorFeature>(false);

  if (start) {
    MockMetricsServer::startFeatures();
  }
}

MockV8Server::MockV8Server(bool start) : MockServer() {
  // setup required application features
  SetupV8Phase(*this);
  addFeature<NetworkFeature>(
      true, _server.getFeature<metrics::MetricsFeature>(),
      network::ConnectionPool::Config{
          .metrics = network::ConnectionPool::Metrics::fromMetricsFeature(
              _server.getFeature<metrics::MetricsFeature>(), "mock")});

  if (start) {
    MockV8Server::startFeatures();
  }
}

MockV8Server::~MockV8Server() {
  if (_server.hasFeature<ClusterFeature>()) {
    _server.getFeature<ClusterFeature>().shutdown();
  }
}

MockAqlServer::MockAqlServer(bool start) : MockServer() {
  // setup required application features
  SetupAqlPhase(*this);

  if (start) {
    MockAqlServer::startFeatures();
  }
}

MockAqlServer::~MockAqlServer() {
  if (_server.hasFeature<ClusterFeature>()) {
    _server.getFeature<ClusterFeature>().shutdown();
  }
  AqlFeature(_server).stop();  // unset singleton instance
}

std::shared_ptr<transaction::Methods> MockAqlServer::createFakeTransaction()
    const {
  std::vector<std::string> noCollections{};
  transaction::Options opts;
  auto ctx = transaction::StandaloneContext::create(
      getSystemDatabase(), transaction::OperationOriginTestCase{});
  return std::make_shared<transaction::Methods>(
      ctx, noCollections, noCollections, noCollections, opts);
}

std::shared_ptr<aql::Query> MockAqlServer::createFakeQuery(
    bool activateTracing, std::string queryString,
    std::function<void(aql::Query&)> callback) const {
  return createFakeQuery(SchedulerFeature::SCHEDULER, activateTracing,
                         queryString, callback);
}

std::shared_ptr<aql::Query> MockAqlServer::createFakeQuery(
    Scheduler* scheduler, bool activateTracing, std::string queryString,
    std::function<void(aql::Query&)> callback) const {
  VPackBuilder queryOptions;
  queryOptions.openObject();
  if (activateTracing) {
    queryOptions.add("profile", VPackValue(int(aql::ProfileLevel::TraceTwo)));
  }
  queryOptions.close();
  if (queryString.empty()) {
    queryString = "RETURN 1";
  }

  auto query = aql::Query::create(
      transaction::StandaloneContext::create(
          getSystemDatabase(), transaction::OperationOriginTestCase{}),
      aql::QueryString(queryString), nullptr,
      aql::QueryOptions(queryOptions.slice()), scheduler);
  callback(*query);
  waitForAsync(query->prepareQuery());

  return query;
}

MockRestServer::MockRestServer(bool start) : MockServer() {
  SetupV8Phase(*this);
  addFeature<QueryRegistryFeature>(
      false, getFeature<arangodb::metrics::MetricsFeature>());
  addFeature<NetworkFeature>(
      true, _server.getFeature<metrics::MetricsFeature>(),
      network::ConnectionPool::Config{
          .metrics = network::ConnectionPool::Metrics::fromMetricsFeature(
              _server.getFeature<metrics::MetricsFeature>(), "mock")});
  if (start) {
    MockRestServer::startFeatures();
  }
}

std::pair<std::vector<consensus::apply_ret_t>, consensus::index_t>
AgencyCache::applyTestTransaction(velocypack::Slice trxs) {
  std::unordered_set<uint64_t> uniq;
  std::vector<uint64_t> toCall;
  std::unordered_set<std::string> pc, cc;
  std::pair<std::vector<consensus::apply_ret_t>, consensus::index_t> res;

  {
    std::lock_guard g(_storeLock);
    ++_commitIndex;
    res = std::pair<std::vector<consensus::apply_ret_t>, consensus::index_t>{
        _readDB.applyTransactions(trxs, AgentInterface::WriteMode{true, true}),
        _commitIndex};  // apply logs
    {
      std::lock_guard g(_callbacksLock);
      for (auto const& trx : VPackArrayIterator(trxs)) {
        handleCallbacksNoLock(trx[0], uniq, toCall, pc, cc);
      }
    }
    for (auto const& i : pc) {
      _planChanges.emplace(_commitIndex, i);
    }
    for (auto const& i : cc) {
      _currentChanges.emplace(_commitIndex, i);
    }
  }

  triggerWaiting(_commitIndex);
  invokeCallbacks(toCall);
  return res;
}

consensus::Store& AgencyCache::store() { return _readDB; }

MockClusterServer::MockClusterServer(bool useAgencyMockPool,
                                     ServerState::RoleEnum newRole,
                                     ServerID serverId,
                                     bool injectClusterIndexes)
    : MockServer(newRole, injectClusterIndexes),
      _useAgencyMockPool(useAgencyMockPool),
      _serverId(serverId) {
  // Add features
  SetupAqlPhase(*this);

  _server.getFeature<ClusterFeature>().allocateMembers();

  addFeature<UpgradeFeature>(false, &_dummy, std::vector<size_t>{});
  addFeature<ServerSecurityFeature>(false);
  addFeature<replication2::replicated_state::ReplicatedStateAppFeature>(false);
  addFeature<ReplicatedLogFeature>(false);

  network::ConnectionPool::Config config;
  config.metrics = network::ConnectionPool::Metrics::fromMetricsFeature(
      _server.getFeature<metrics::MetricsFeature>(), "network-mock");
  config.numIOThreads = 1;
  config.maxOpenConnections = 8;
  config.verifyHosts = false;
  addFeature<NetworkFeature>(
      true, _server.getFeature<metrics::MetricsFeature>(), config);
}

MockClusterServer::~MockClusterServer() {
  _server.getFeature<ClusterFeature>().shutdown();
}

void MockClusterServer::startFeatures() {
  MockServer::startFeatures();

  network::ConnectionPool::Config poolConfig;
  poolConfig.clusterInfo = &getFeature<ClusterFeature>().clusterInfo();
  poolConfig.numIOThreads = 1;
  poolConfig.maxOpenConnections = 3;
  poolConfig.verifyHosts = false;
  poolConfig.metrics = network::ConnectionPool::Metrics::fromMetricsFeature(
      _server.getFeature<metrics::MetricsFeature>(), "mock");

  if (_useAgencyMockPool) {
    _pool = std::make_unique<AsyncAgencyStorePoolMock>(_server, poolConfig);
  } else {
    _pool = std::make_unique<PreparedResponseConnectionPool>(
        _server.getFeature<ClusterFeature>().agencyCache(), poolConfig);

    // Inject the faked Pool into NetworkFeature
    _server.getFeature<NetworkFeature>().setPoolTesting(_pool.get());
  }

  AgencyCommHelper::initialize("arango");
  AsyncAgencyCommManager::initialize(server());
  AsyncAgencyCommManager::INSTANCE->pool(_pool.get());
  AsyncAgencyCommManager::INSTANCE->updateEndpoints({"tcp://localhost:4000/"});
  AgencyComm(server()).ensureStructureInitialized();
  std::string st =
      "{\"" + ServerState::instance()->getId() + "\":{\"rebootId\":1}}";
  agencyTrx("/arango/Current/ServersKnown", st);
  ServerState::instance()->setRebootId(RebootId{1});

  // register factories & normalizers
  auto& indexFactory = const_cast<IndexFactory&>(_engine->indexFactory());
  auto& factory =
      getFeature<iresearch::IResearchFeature>().factory<ClusterEngine>();
  indexFactory.emplace(
      std::string{iresearch::StaticStrings::ViewArangoSearchType}, factory);
  _server.getFeature<ClusterFeature>().clusterInfo().startSyncers();
}

std::shared_ptr<aql::Query> MockClusterServer::createFakeQuery(
    bool activateTracing, std::string queryString,
    std::function<void(aql::Query&)> callback) const {
  VPackBuilder queryOptions;
  queryOptions.openObject();
  if (activateTracing) {
    queryOptions.add("profile", VPackValue(int(aql::ProfileLevel::TraceTwo)));
  }
  queryOptions.close();
  if (queryString.empty()) {
    queryString = "RETURN 1";
  }

  auto query = aql::Query::create(
      transaction::StandaloneContext::create(
          getSystemDatabase(), transaction::OperationOriginTestCase{}),
      aql::QueryString(queryString), nullptr,
      aql::QueryOptions(queryOptions.slice()));
  callback(*query);
  waitForAsync(query->prepareQuery());

  return query;
}

consensus::index_t MockClusterServer::agencyTrx(std::string const& key,
                                                std::string const& value) {
  // Build an agency transaction:
  VPackBuilder b;
  {
    VPackArrayBuilder trxs(&b);
    {
      VPackArrayBuilder trx(&b);
      {
        VPackObjectBuilder op(&b);
        auto b2 = VPackParser::fromJson(value);
        b.add(key, b2->slice());
      }
    }
  }
  return std::get<1>(
      _server.getFeature<ClusterFeature>().agencyCache().applyTestTransaction(
          b.slice()));
}

void MockClusterServer::agencyCreateDatabase(std::string const& name) {
  TemplateSpecializer ts(name, [&]() { return genUniqId(); });
  std::string st = ts.specialize(plan_dbs_string);

  agencyTrx("/arango/Plan/Databases/" + name, st);
  st = ts.specialize(current_dbs_string);
  agencyTrx("/arango/Current/Databases/" + name, st);

  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForPlan(agencyTrx("/arango/Plan/Version", R"=({"op":"increment"})="))
      .wait();
  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForCurrent(
          agencyTrx("/arango/Current/Version", R"=({"op":"increment"})="))
      .wait();
}

void MockClusterServer::agencyCreateCollections(std::string const& name) {
  TemplateSpecializer ts(name, [&]() { return genUniqId(); });
  std::string st = ts.specialize(plan_colls_string);
  agencyTrx("/arango/Plan/Collections/" + name, st);
  st = ts.specialize(current_colls_string);
  agencyTrx("/arango/Current/Collections/" + name, st);

  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForPlan(agencyTrx("/arango/Plan/Version", R"=({"op":"increment"})="))
      .wait();
  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForCurrent(
          agencyTrx("/arango/Current/Version", R"=({"op":"increment"})="))
      .wait();
}

void MockClusterServer::agencyDropDatabase(std::string const& name) {
  std::string st = R"=({"op":"delete"}))=";

  agencyTrx("/arango/Plan/Databases/" + name, st);
  agencyTrx("/arango/Plan/Collections/" + name, st);
  agencyTrx("/arango/Current/Databases/" + name, st);
  agencyTrx("/arango/Current/Collections/" + name, st);

  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForPlan(agencyTrx("/arango/Plan/Version", R"=({"op":"increment"})="))
      .wait();
  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForCurrent(
          agencyTrx("/arango/Current/Version", R"=({"op":"increment"})="))
      .wait();
}

ServerID const& MockClusterServer::getServerID() const { return _serverId; }

void MockClusterServer::buildCollectionProperties(
    VPackBuilder& props, std::string const& collectionName,
    std::string const& cid, TRI_col_type_e type,
    VPackSlice additionalProperties) {
  {
    // This is hand-crafted unfortunately the code does not exist...
    VPackObjectBuilder guard(&props);
    props.add(StaticStrings::DataSourceType, VPackValue(type));
    props.add(StaticStrings::DataSourceName, VPackValue(collectionName));
    props.add(StaticStrings::DataSourcePlanId, VPackValue(cid));
    props.add(StaticStrings::DataSourceId, VPackValue(cid));
    props.add(VPackValue(StaticStrings::Indexes));
    {
      VPackArrayBuilder guard2(&props);
      auto const primIndex = velocypack::Parser::fromJson(
          R"({"id":"0","type":"primary","name":
"primary","fields":["_key"],"unique":true,"sparse":false
})");
      props.add(primIndex->slice());
      if (type == TRI_COL_TYPE_EDGE) {
        auto const fromIndex = velocypack::Parser::fromJson(
            R"({"id":"1","type":"edge","name":
"edge_from","fields":["_from"],"unique":false,"sparse":false
})");
        props.add(fromIndex->slice());
        auto const toIndex = velocypack::Parser::fromJson(
            R"({"id":"2","type":"edge","name":
"edge_to","fields":["_to"],"unique":false,"sparse":false})");
        props.add(toIndex->slice());
      }
    }

    if (additionalProperties.isObject()) {
      props.add(VPackObjectIterator(additionalProperties));
    }
  }
}

void MockClusterServer::injectCollectionToAgency(
    std::string const& dbName, VPackBuilder& velocy, DataSourceId const& planId,
    std::vector<std::pair<std::string, std::string>>
        shardNameToServerNamePairs) {
  agencyTrx("/arango/Plan/Collections/" + dbName + "/" +
                basics::StringUtils::itoa(planId.id()),
            velocy.toJson());
  {
    /* Hard-Coded section to inject the CURRENT counter part.
     * We do not have a shard available here that could generate the values
     * accordingly.
     */
    VPackBuilder current;
    {
      VPackObjectBuilder report(&current);
      for (auto const& [shard, server] : shardNameToServerNamePairs) {
        current.add(VPackValue(shard));
        VPackObjectBuilder shardReport(&current);
        current.add(VPackValue(maintenance::SERVERS));
        {
          VPackArrayBuilder serverList(&current);
          current.add(VPackValue(server));
        }
        current.add(VPackValue(StaticStrings::FailoverCandidates));
        {
          VPackArrayBuilder serverList(&current);
          current.add(VPackValue(server));
        }
        // Always no error
        current.add(StaticStrings::Error, VPackValue(false));
        current.add(StaticStrings::ErrorMessage, VPackValue(std::string()));
        current.add(StaticStrings::ErrorNum, VPackValue(0));
        // NOTE: we omited Indexes
      }
    }
    agencyTrx("/arango/Current/Collections/" + dbName + "/" +
                  basics::StringUtils::itoa(planId.id()),
              current.toJson());
  }

  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForPlan(agencyTrx("/arango/Plan/Version", R"=({"op":"increment"})="))
      .wait();

  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForCurrent(
          agencyTrx("/arango/Current/Version", R"=({"op":"increment"})="))
      .wait();
}

// Create a clusterWide Collection.
// This does NOT create Shards.
std::shared_ptr<LogicalCollection> MockClusterServer::createCollection(
    std::string const& dbName, std::string collectionName,
    std::vector<std::pair<std::string, std::string>> shardNameToServerNamePairs,
    TRI_col_type_e type, VPackSlice additionalProperties) {
  std::string cid = std::to_string(
      _server.getFeature<ClusterFeature>().clusterInfo().uniqid());
  auto& databaseFeature = _server.getFeature<DatabaseFeature>();
  auto vocbase = databaseFeature.lookupDatabase(dbName);

  VPackBuilder props;
  buildCollectionProperties(props, collectionName, cid, type,
                            additionalProperties);
  LogicalCollection dummy(*vocbase, props.slice(), true);

  auto shards = std::make_shared<ShardMap>();
  for (auto const& [shard, server] : shardNameToServerNamePairs) {
    shards->emplace(shard, std::vector<ServerID>{server});
  }
  dummy.setShardMap(shards);

  std::unordered_set<std::string> const ignoreKeys{
      "allowUserKeys", "cid",     "globallyUniqueId", "count",
      "planId",        "version", "objectId"};
  VPackBuilder velocy = dummy.toVelocyPackIgnore(
      ignoreKeys, LogicalDataSource::Serialization::List);
  injectCollectionToAgency(dbName, velocy, dummy.planId(),
                           shardNameToServerNamePairs);

  ClusterInfo& clusterInfo =
      server().getFeature<ClusterFeature>().clusterInfo();
  return clusterInfo.getCollection(dbName, collectionName);
}

MockDBServer::MockDBServer(ServerID serverId, bool start, bool useAgencyMock)
    : MockClusterServer(useAgencyMock, ServerState::RoleEnum::ROLE_DBSERVER,
                        serverId) {
  addFeature<FlushFeature>(false);        // do not start the thread
  addFeature<MaintenanceFeature>(false);  // do not start the thread

  // turn off auto-repairing of revision trees for unit tests
  auto& rf = addFeature<arangodb::ReplicationFeature>(false);  // do not start
  rf.autoRepairRevisionTrees(false);

  if (start) {
    MockDBServer::startFeatures();
    MockDBServer::createDatabase("_system");
  }
  ServerState::instance()->setId(serverId);
}

MockDBServer::~MockDBServer() = default;

TRI_vocbase_t* MockDBServer::createDatabase(std::string const& name) {
  agencyCreateDatabase(name);
  // Now we must run a maintenance action to create the database locally,
  // unless it is the system database, in which case this does not work:
  if (name != "_system") {
    maintenance::ActionDescription ad(
        {{std::string(maintenance::NAME),
          std::string(maintenance::CREATE_DATABASE)},
         {std::string(maintenance::DATABASE), std::string(name)}},
        maintenance::HIGHER_PRIORITY, false);
    auto& mf = _server.getFeature<MaintenanceFeature>();
    maintenance::CreateDatabase cd(mf, ad);
    cd.first();  // Does the job
  }
  agencyCreateCollections(name);

  auto& databaseFeature = _server.getFeature<DatabaseFeature>();
  auto vocbase = databaseFeature.lookupDatabase(name);
  TRI_ASSERT(vocbase != nullptr);
  return vocbase;
};

void MockDBServer::dropDatabase(std::string const& name) {
  agencyDropDatabase(name);
  auto& databaseFeature = _server.getFeature<DatabaseFeature>();
  auto vocbase = databaseFeature.lookupDatabase(name);
  TRI_ASSERT(vocbase == nullptr);

  // Now we must run a maintenance action to create the database locally:
  maintenance::ActionDescription ad(
      {{std::string(maintenance::NAME),
        std::string(maintenance::DROP_DATABASE)},
       {std::string(maintenance::DATABASE), std::string(name)}},
      maintenance::HIGHER_PRIORITY, false);
  auto& mf = _server.getFeature<MaintenanceFeature>();
  maintenance::DropDatabase dd(mf, ad);
  dd.first();  // Does the job
}

void MockDBServer::createShard(std::string const& dbName,
                               std::string const& shardName,
                               LogicalCollection& clusterCollection) {
  auto props = std::make_shared<VPackBuilder>();
  {
    // This is hand-crafted unfortunately the code does not exist...
    VPackObjectBuilder guard(props.get());
    props->add(StaticStrings::DataSourceType,
               VPackValue(clusterCollection.type()));
    props->add(StaticStrings::DataSourceName, VPackValue(shardName));
    // We are in SingleMachine test code. Setting a replicationFactor > 2 here
    // Would cause us to get stuck on writes.
    // We may allow this for tests that do not write documents into the
    // collection.
    TRI_ASSERT(clusterCollection.replicationFactor() < 2);
    props->add(StaticStrings::ReplicationFactor,
               VPackValue(clusterCollection.replicationFactor()));
    props->add(StaticStrings::InternalValidatorTypes,
               VPackValue(clusterCollection.getInternalValidatorTypes()));
  }
  maintenance::ActionDescription ad(
      std::map<std::string, std::string>{
          {maintenance::NAME, maintenance::CREATE_COLLECTION},
          {maintenance::COLLECTION,
           basics::StringUtils::itoa(clusterCollection.planId().id())},
          {maintenance::SHARD, shardName},
          {maintenance::DATABASE, dbName},
          {maintenance::SERVER_ID, ServerState::instance()->getId()},
          {maintenance::THE_LEADER, ""}},
      maintenance::HIGHER_PRIORITY, false, props);

  auto& mf = _server.getFeature<MaintenanceFeature>();
  maintenance::CreateCollection dd(mf, ad);
  bool work = dd.first();
  // Managed to create the collection, if this is true we did not manage to
  // create the collections. We can investigate Result here. We may need to call
  // next()
  TRI_ASSERT(work == false);

  // If this is false something above went wrong.
  TRI_ASSERT(dd.ok());

  // Add Indexes:
  // The Mock does not support generating INdexes from setup JSON.
  // It only supports manual index creation,
  if (clusterCollection.type() == TRI_COL_TYPE_EDGE) {
    auto& databaseFeature = _server.getFeature<DatabaseFeature>();
    auto vocbase = databaseFeature.lookupDatabase(dbName);
    TRI_ASSERT(vocbase);
    auto col = vocbase->lookupCollection(shardName);
    // We just created it...
    TRI_ASSERT(col);

    {
      bool created = false;
      auto const idx = velocypack::Parser::fromJson(
          R"({"id":"1","type":"edge","name":"edge_from","fields":["_from"],"unique":false,"sparse":false})");
      col->createIndex(idx->slice(), created).waitAndGet();
      TRI_ASSERT(created);
    }

    {
      bool created = false;
      auto const idx = velocypack::Parser::fromJson(
          R"({"id":"2","type":"edge","name":"edge_to","fields":["_to"],"unique":false,"sparse":false})");
      col->createIndex(idx->slice(), created).waitAndGet();
      TRI_ASSERT(created);
    }
  }
}

MockCoordinator::MockCoordinator(ServerID serverId, bool start,
                                 bool useAgencyMock, bool injectClusterIndexes)
    : MockClusterServer(useAgencyMock, ServerState::RoleEnum::ROLE_COORDINATOR,
                        serverId, injectClusterIndexes) {
  addFeature<arangodb::metrics::ClusterMetricsFeature>(false).disable();
  if (start) {
    MockCoordinator::startFeatures();
    MockCoordinator::createDatabase("_system");
    agencyTrx(
        "/.agency",
        R"=({"op":"set", "new":{"timeoutMult":1,"term":1,"size":3,"pool":{"AGNT-ca355865-7e34-40b8-91d4-198811e52f44":"tcp://[::1]:4001","AGNT-93fa47f7-9f79-493e-b2da-b74487baccae":"tcp://[::1]:4003","AGNT-93908f62-5414-4456-be37-2226651b8358":"tcp://[::1]:4002"},"id":"AGNT-ca355865-7e34-40b8-91d4-198811e52f44","active":["AGNT-93908f62-5414-4456-be37-2226651b8358","AGNT-ca355865-7e34-40b8-91d4-198811e52f44","AGNT-93fa47f7-9f79-493e-b2da-b74487baccae"]}})=");
  }
}

MockCoordinator::~MockCoordinator() = default;

std::pair<std::string, std::string> MockCoordinator::registerFakedDBServer(
    std::string const& serverName) {
  VPackBuilder builder;
  std::string fakedHost = "invalid-url-type-name";
  std::string fakedPort = "98234";
  std::string fakedEndpoint = "tcp://" + fakedHost + ":" + fakedPort;
  {
    VPackObjectBuilder b(&builder);
    builder.add("endpoint", VPackValue(fakedEndpoint));
    builder.add("advertisedEndpoint", VPackValue(fakedEndpoint));
    builder.add("host", VPackValue(fakedHost));
    builder.add("version",
                VPackValue(rest::Version::getNumericServerVersion()));
    builder.add("versionString", VPackValue(rest::Version::getServerVersion()));
    builder.add("engine", VPackValue("testEngine"));
    builder.add(
        "timestamp",
        VPackValue(timepointToString(std::chrono::system_clock::now())));
  }
  agencyTrx("/arango/Current/ServersRegistered/" + serverName,
            builder.toJson());
  _server.getFeature<ClusterFeature>()
      .clusterInfo()
      .waitForCurrent(
          agencyTrx("/arango/Current/Version", R"=({"op":"increment"})="))
      .wait();
  return std::make_pair(fakedHost, fakedPort);
}

TRI_vocbase_t* MockCoordinator::createDatabase(std::string const& name) {
  agencyCreateDatabase(name);
  agencyCreateCollections(name);
  auto& databaseFeature = _server.getFeature<DatabaseFeature>();
  auto vocbase = databaseFeature.lookupDatabase(name);
  TRI_ASSERT(vocbase != nullptr);
  return vocbase;
};

void MockCoordinator::dropDatabase(std::string const& name) {
  agencyDropDatabase(name);
  auto& databaseFeature = _server.getFeature<DatabaseFeature>();
  auto vocbase = databaseFeature.lookupDatabase(name);
  TRI_ASSERT(vocbase == nullptr);
}

network::ConnectionPool* MockCoordinator::getPool() { return _pool.get(); }

MockRestAqlServer::MockRestAqlServer() {
  SetupAqlPhase(*this);
  addFeature<NetworkFeature>(
      true, _server.getFeature<metrics::MetricsFeature>(),
      network::ConnectionPool::Config{
          .metrics = network::ConnectionPool::Metrics::fromMetricsFeature(
              _server.getFeature<metrics::MetricsFeature>(), "mock")});
  MockRestAqlServer::startFeatures();
}
