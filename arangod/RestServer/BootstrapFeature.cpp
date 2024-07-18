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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "RestServer/BootstrapFeature.h"

#include "Agency/AgencyComm.h"
#include "Agency/AsyncAgencyComm.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryList.h"
#include "Auth/UserManager.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterUpgradeFeature.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "Rest/GeneralResponse.h"
#include "Rest/Version.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/Methods/Upgrade.h"
#include "VocBase/vocbase.h"

#include <velocypack/Iterator.h>

namespace {
static std::string const bootstrapKey = "Bootstrap";
static std::string const healthKey = "Supervision/Health";
}  // namespace

namespace arangodb {
namespace aql {
class Query;
}
}  // namespace arangodb

using namespace arangodb;
using namespace arangodb::options;

BootstrapFeature::BootstrapFeature(Server& server)
    : ArangodFeature{server, *this}, _isReady(false), _bark(false) {
  startsAfter<application_features::ServerFeaturePhase>();

  startsAfter<SystemDatabaseFeature>();

#ifdef USE_V8
  // TODO: It is only in FoxxPhase because of:
  startsAfter<FoxxFeature>();
#else
  startsAfter<application_features::ServerFeaturePhase>();
#endif

  // If this is Sorted out we can go down to ServerPhase
  // And activate the following dependencies:
  /*
  startsAfter("Endpoint");
  startsAfter("GeneralServer");
  startsAfter("Server");
  startsAfter("Upgrade");
  */
}

bool BootstrapFeature::isReady() const {
  TRI_IF_FAILURE("BootstrapFeature_not_ready") { return false; }
  return _isReady;
}

void BootstrapFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addOption(
      "--hund", "Make ArangoDB bark on startup.", new BooleanParameter(&_bark),
      arangodb::options::makeDefaultFlags(arangodb::options::Flags::Uncommon));
}

// Local Helper functions
namespace {

/// Initialize certain agency entries, like Plan, system collections
/// and various similar things. Only runs through on a SINGLE coordinator.
/// must only return if we are bootstrap lead or bootstrap is done.
void raceForClusterBootstrap(BootstrapFeature& feature) {
  AgencyComm agency(feature.server());
  auto& ci = feature.server().getFeature<ClusterFeature>().clusterInfo();
  while (!feature.server().isStopping()) {
    AgencyCommResult result = agency.getValues(::bootstrapKey);
    if (!result.successful()) {
      // Error in communication, note that value not found is not an error
      LOG_TOPIC("2488f", TRACE, Logger::STARTUP)
          << "raceForClusterBootstrap: no agency communication";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    VPackSlice value = result.slice()[0].get(
        std::vector<std::string>({AgencyCommHelper::path(), ::bootstrapKey}));
    if (value.isString()) {
      // key was found and is a string
      std::string bootstrapVal = value.copyString();
      if (bootstrapVal.find("done") != std::string::npos) {
        // all done, let's get out of here:
        LOG_TOPIC("61e04", TRACE, Logger::STARTUP)
            << "raceForClusterBootstrap: bootstrap already done";
        return;
      } else if (bootstrapVal == ServerState::instance()->getId()) {
        agency.removeValues(::bootstrapKey, false);
      }
      LOG_TOPIC("49437", DEBUG, Logger::STARTUP)
          << "raceForClusterBootstrap: somebody else does the bootstrap";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    // No value set, we try to do the bootstrap ourselves:
    VPackBuilder b;
    b.add(VPackValue(arangodb::ServerState::instance()->getId()));
    result = agency.casValue(::bootstrapKey, b.slice(), false, 300, 15);
    if (!result.successful()) {
      LOG_TOPIC("a1ecb", DEBUG, Logger::STARTUP)
          << "raceForClusterBootstrap: lost race, somebody else will bootstrap";
      // Cannot get foot into the door, try again later:
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    // OK, we handle things now
    LOG_TOPIC("784e2", DEBUG, Logger::STARTUP)
        << "raceForClusterBootstrap: race won, we do the bootstrap";

    // let's see whether a DBserver is there:
    ci.loadCurrentDBServers();

    auto dbservers = ci.getCurrentDBServers();

    if (dbservers.size() == 0) {
      LOG_TOPIC("0ad1c", TRACE, Logger::STARTUP)
          << "raceForClusterBootstrap: no DBservers, waiting";
      agency.removeValues(::bootstrapKey, false);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    arangodb::SystemDatabaseFeature::ptr vocbase =
        feature.server().hasFeature<arangodb::SystemDatabaseFeature>()
            ? feature.server()
                  .getFeature<arangodb::SystemDatabaseFeature>()
                  .use()
            : nullptr;
    auto upgradeRes =
        vocbase ? methods::Upgrade::clusterBootstrap(*vocbase).result()
                : arangodb::Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);

    if (upgradeRes.fail()) {
      LOG_TOPIC("8903f", ERR, Logger::STARTUP)
          << "Problems with cluster bootstrap, "
          << "marking as not successful.";
      agency.removeValues(::bootstrapKey, false);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    // become Foxxmaster, ignore result
    LOG_TOPIC("00162", DEBUG, Logger::STARTUP) << "Write Foxxmaster";
    agency.setValue("Current/Foxxmaster", b.slice(), 0);
    agency.increment("Current/Version");

    LOG_TOPIC("571fb", DEBUG, Logger::STARTUP) << "Creating the root user";
    auth::UserManager* um = AuthenticationFeature::instance()->userManager();
    if (um != nullptr) {
      um->createRootUser();
    }

    LOG_TOPIC("ad91d", DEBUG, Logger::STARTUP)
        << "raceForClusterBootstrap: bootstrap done";

    b.clear();
    b.add(VPackValue(arangodb::ServerState::instance()->getId() + ": done"));
    result = agency.setValue(::bootstrapKey, b.slice(), 0);
    if (result.successful()) {
      // store current version number in agency to avoid unnecessary upgrades
      // to the same version
      if (feature.server().hasFeature<ClusterUpgradeFeature>()) {
        ClusterUpgradeFeature& clusterUpgradeFeature =
            feature.server().getFeature<ClusterUpgradeFeature>();
        clusterUpgradeFeature.setBootstrapVersion();
      }
      return;
    }

    LOG_TOPIC("04fb7", TRACE, Logger::STARTUP)
        << "raceForClusterBootstrap: could not indicate success";
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

#ifdef USE_V8
/// Run the coordinator initialization script, will run on each
/// coordinator, not just one.
void runCoordinatorJS(TRI_vocbase_t* vocbase, BootstrapFeature& feature) {
  bool success = false;
  while (!success && !feature.server().isStopping()) {
    LOG_TOPIC("0f953", DEBUG, Logger::STARTUP)
        << "Running server/bootstrap/coordinator.js";

    VPackBuilder builder;
    vocbase->server()
        .getFeature<V8DealerFeature>()
        .loadJavaScriptFileInAllExecutors(
            vocbase, "server/bootstrap/coordinator.js", &builder);

    auto slice = builder.slice();
    if (slice.isArray()) {
      if (slice.length() > 0) {
        bool newResult = true;
        for (VPackSlice val : VPackArrayIterator(slice)) {
          newResult = newResult && val.isTrue();
        }
        if (!newResult) {
          LOG_TOPIC("6ca4b", WARN, Logger::STARTUP)
              << "result of bootstrap was: " << builder.toJson()
              << ". retrying bootstrap in 1s.";
        }
        success = newResult;
      } else {
        LOG_TOPIC("541a2", WARN, Logger::STARTUP)
            << "bootstrap wasn't executed in a single context! retrying "
               "bootstrap in 1s.";
      }
    } else {
      LOG_TOPIC("5f716", WARN, Logger::STARTUP)
          << "result of bootstrap was not an array: " << slice.typeName()
          << ". retrying bootstrap in 1s.";
    }
    if (!success) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}
#endif

}  // namespace

void BootstrapFeature::start() {
  auto& databaseFeature = server().getFeature<DatabaseFeature>();

  arangodb::SystemDatabaseFeature::ptr vocbase =
      server().hasFeature<arangodb::SystemDatabaseFeature>()
          ? server().getFeature<arangodb::SystemDatabaseFeature>().use()
          : nullptr;
#ifdef USE_V8
  bool v8Enabled = server().hasFeature<V8DealerFeature>() &&
                   server().isEnabled<V8DealerFeature>() &&
                   server().getFeature<V8DealerFeature>().isEnabled();
#endif
  TRI_ASSERT(vocbase.get() != nullptr);

  ServerState::RoleEnum role = ServerState::instance()->getRole();

  if (ServerState::isRunningInCluster(role)) {
    // the coordinators will race to perform the cluster initialization.
    // The coordinatpr who does it will create system collections and
    // the root user
    if (ServerState::isCoordinator(role)) {
      LOG_TOPIC("724e0", DEBUG, Logger::STARTUP)
          << "Racing for cluster bootstrap...";
      // note: this may create the _system database in Plan!
      raceForClusterBootstrap(*this);

      // wait until at least one database appears. this is an indication that
      // both Plan and Current have been populated successfully
      waitForDatabases();

#ifdef USE_V8
      if (v8Enabled && !databaseFeature.upgrade()) {
        ::runCoordinatorJS(vocbase.get(), *this);
      }
#endif
    } else if (ServerState::isDBServer(role)) {
      // don't wait for databases in Current here, as we are a DB server and may
      // be the one responsible to create it. blocking here is thus no option!

      LOG_TOPIC("a2b65", DEBUG, Logger::STARTUP) << "Running bootstrap";

      auto upgradeRes = methods::Upgrade::clusterBootstrap(*vocbase);

      if (upgradeRes.fail()) {
        LOG_TOPIC("4e67f", ERR, Logger::STARTUP)
            << "Problem during startup: " << upgradeRes.errorMessage();
      }
    } else {
      TRI_ASSERT(false);
    }
  } else {
    std::string const myId =
        ServerState::instance()->getId();  // local cluster UUID

    // become leader before running server.js to ensure the leader
    // is the foxxmaster. Everything else is handled in heartbeat
    ServerState::instance()->setFoxxmaster(
        myId);  // could be empty, but set anyway

#ifdef USE_V8
    if (v8Enabled) {  // runs the single server bootstrap JS
      // will run foxx/manager.js::_startup() and more (start queues, load
      // routes, etc)
      LOG_TOPIC("e0c8b", DEBUG, Logger::STARTUP) << "Running server/server.js";
      server().getFeature<V8DealerFeature>().loadJavaScriptFileInAllExecutors(
          vocbase.get(), "server/server.js", nullptr);
    }
#endif
    auth::UserManager* um = AuthenticationFeature::instance()->userManager();
    if (um != nullptr) {
      // only creates root user if it does not exist, will be overwritten on
      // slaves
      um->createRootUser();
    }
  }

  if (ServerState::isClusterRole(role)) {
    waitForHealthEntry();
  }

  // Start service properly:
  ServerState::setServerMode(ServerState::Mode::DEFAULT);

  if (!databaseFeature.upgrade()) {
    LOG_TOPIC("cf3f4", INFO, arangodb::Logger::FIXME)
        << "ArangoDB (version " << ARANGODB_VERSION_FULL
        << ") is ready for business. Have fun!";
  }

  if (_bark) {
    LOG_TOPIC("bb9b7", INFO, arangodb::Logger::FIXME)
        << "The dog says: Гав гав";
  }

  _isReady = true;
}

void BootstrapFeature::unprepare() {
  // notify all currently running queries about the shutdown
  auto& databaseFeature = server().getFeature<DatabaseFeature>();

  for (auto& name : databaseFeature.getDatabaseNames()) {
    auto vocbase = databaseFeature.useDatabase(name);

    if (vocbase != nullptr) {
      vocbase->queryList()->kill([](aql::Query&) { return true; }, true);
    }
  }
}

void BootstrapFeature::waitForHealthEntry() {
  LOG_TOPIC("4000c", DEBUG, arangodb::Logger::CLUSTER)
      << "waiting for our health entry to appear in Supervision/Health";
  bool found = false;
  AgencyComm agency(server());
  int tries = 0;
  while (++tries < 30) {
    AgencyCommResult result = agency.getValues(::healthKey);
    if (result.successful()) {
      VPackSlice value = result.slice()[0].get(std::vector<std::string>(
          {AgencyCommHelper::path(), "Supervision", "Health",
           ServerState::instance()->getId(), "Status"}));
      if (value.isString() && !value.stringView().empty()) {
        found = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (found) {
    LOG_TOPIC("b0de6", DEBUG, arangodb::Logger::CLUSTER)
        << "found our health entry in Supervision/Health";
  } else {
    LOG_TOPIC("2c993", INFO, arangodb::Logger::CLUSTER)
        << "did not find our health entry after 15 s in Supervision/Health";
  }
}

void BootstrapFeature::waitForDatabases() const {
  auto& ci = server().getFeature<ClusterFeature>().clusterInfo();

  uint64_t iterations = 0;
  while (ci.databases().empty() && !server().isStopping()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    if (++iterations % 2000 == 0) {
      // log every few seconds that we are waiting here
      LOG_TOPIC("db886", INFO, Logger::CLUSTER)
          << "waiting for databases to appear...";
    }
  }
}
