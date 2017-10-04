////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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

#include "ReplicationFeature.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Replication/GlobalReplicationApplier.h"
#include "Replication/ReplicationApplierConfiguration.h"
#include "VocBase/vocbase.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::options;

ReplicationFeature* ReplicationFeature::INSTANCE = nullptr;

ReplicationFeature::ReplicationFeature(ApplicationServer* server)
    : ApplicationFeature(server, "Replication"),
      _replicationApplier(true) {

  setOptional(false);
  requiresElevatedPrivileges(false);
  startsAfter("Database");
  startsAfter("StorageEngine");
}

void ReplicationFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("database", "Configure the database");

  options->addOldOption("server.disable-replication-applier",
                        "database.replication-applier");
  
  options->addHiddenOption(
      "--database.replication-applier",
      "switch to enable or disable the replication applier",
      new BooleanParameter(&_replicationApplier));
}

void ReplicationFeature::validateOptions(std::shared_ptr<options::ProgramOptions> options) {}

void ReplicationFeature::prepare() { 
  INSTANCE = this;
}

void ReplicationFeature::start() { 
  // TODO: pass proper configuration into applier
  _globalReplicationApplier.reset(new GlobalReplicationApplier(ReplicationApplierConfiguration()));
}

void ReplicationFeature::beginShutdown() {
  try {
    if (_globalReplicationApplier != nullptr) {
      _globalReplicationApplier->stop(true);
    }
  } catch (...) {
    // ignore any error
  }
}

void ReplicationFeature::stop() {}

void ReplicationFeature::unprepare() {
  _globalReplicationApplier.reset();
}
    
// start the replication applier for a single database
void ReplicationFeature::startApplier(TRI_vocbase_t* vocbase) {
  TRI_ASSERT(vocbase->type() == TRI_VOCBASE_TYPE_NORMAL);
  TRI_ASSERT(vocbase->replicationApplier() != nullptr);

  if (vocbase->replicationApplier()->autoStart()) {
    if (!_replicationApplier) {
      LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "replication applier explicitly deactivated for database '"
                << vocbase->name() << "'";
    } else {
      try {
        vocbase->replicationApplier()->start(0, false, 0);
      } catch (std::exception const& ex) {
        LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "unable to start replication applier for database '"
                  << vocbase->name() << "': " << ex.what();
      } catch (...) {
        LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "unable to start replication applier for database '"
                  << vocbase->name() << "'";
      }
    }
  }
}

// stop the replication applier for a single database
void ReplicationFeature::stopApplier(TRI_vocbase_t* vocbase) {
  TRI_ASSERT(vocbase->type() == TRI_VOCBASE_TYPE_NORMAL);

  if (vocbase->replicationApplier() != nullptr) {
    vocbase->replicationApplier()->stopAndJoin(false);
  }
}
