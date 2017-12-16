////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_CLUSTER_CLUSTER_FEATURE_H
#define ARANGOD_CLUSTER_CLUSTER_FEATURE_H 1

#include "Basics/Common.h"

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Cluster/ServerState.h"

namespace arangodb {
class AgencyCallbackRegistry;
class HeartbeatThread;

class ClusterFeature : public application_features::ApplicationFeature {
 public:
  explicit ClusterFeature(application_features::ApplicationServer*);
  ~ClusterFeature();

 public:
  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override final;
  void start() override final;
  void beginShutdown() override final;
  void unprepare() override final;

  std::vector<std::string> agencyEndpoints() const {
    return _agencyEndpoints;
  }

  std::string agencyPrefix() {
    return _agencyPrefix;
  }

 private:
  std::vector<std::string> _agencyEndpoints;
  std::string _agencyPrefix;
  std::string _myRole;
  std::string _myAddress;
  uint32_t _systemReplicationFactor = 2;
  bool _createWaitsForSyncReplication = true;

 private:
  void reportRole(ServerState::RoleEnum);

 public:
  AgencyCallbackRegistry* agencyCallbackRegistry() const {
    return _agencyCallbackRegistry.get();
  }

  std::string const agencyCallbacksPath() const {
    return "/_api/agency/agency-callbacks";
  };

  std::string const clusterRestPath() const {
    return "/_api/cluster";
  };

  void setUnregisterOnShutdown(bool);
  bool createWaitsForSyncReplication() { return _createWaitsForSyncReplication; };
  uint32_t systemReplicationFactor() { return _systemReplicationFactor; };

  void stop() override final;

 private:
  bool _unregisterOnShutdown;
  bool _enableCluster;
  std::shared_ptr<HeartbeatThread> _heartbeatThread;
  uint64_t _heartbeatInterval;
  bool _disableHeartbeat;
  std::unique_ptr<AgencyCallbackRegistry> _agencyCallbackRegistry;
  ServerState::RoleEnum _requestedRole;
  // FIXME: remove in > 3.3
  std::string _myLocalInfo;
};
}

#endif
