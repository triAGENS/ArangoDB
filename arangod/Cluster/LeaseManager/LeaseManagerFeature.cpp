////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
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

#include "LeaseManagerFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/voc-errors.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/LeaseManager/LeaseManager.h"
#include "Cluster/LeaseManager/LeaseManagerNetworkHandler.h"
#include "Network/NetworkFeature.h"
#include "Scheduler/SchedulerFeature.h"

using namespace arangodb::cluster;

LeaseManagerFeature::LeaseManagerFeature(Server& server,
                                         ClusterFeature& clusterFeature,
                                         NetworkFeature& networkFeature,
                                         SchedulerFeature& schedulerFeature)
    : ArangodFeature{server, *this},
      _clusterFeature(clusterFeature),
      _networkFeature(networkFeature),
      _schedulerFeature(schedulerFeature) {
  setOptional(true);
  startsAfter<ClusterFeature>();
  startsAfter<NetworkFeature>();
  startsAfter<SchedulerFeature>();
  onlyEnabledWith<ClusterFeature>();
  onlyEnabledWith<NetworkFeature>();
  onlyEnabledWith<SchedulerFeature>();
}

void LeaseManagerFeature::prepare() {
  // If this throws the ClusterFeature was not started properly.
  // We have an issue with startup ordering
  auto& ci = _clusterFeature.clusterInfo();
  auto pool = _networkFeature.pool();
  ADB_PROD_ASSERT(pool != nullptr)
      << "Issue with startup ordering of features: NetworkFeature not yet "
         "started.";
  ADB_PROD_ASSERT(SchedulerFeature::SCHEDULER != nullptr)
      << "Issue with startup ordering of features: SchedulerFeature not yet "
         "started.";

  // Allocate the LeaseManager.
  // This must be done after the NetworkFeature has been prepared.
  _leaseManager = std::make_unique<cluster::LeaseManager>(
      ci.rebootTracker(),
      std::make_unique<cluster::LeaseManagerNetworkHandler>(pool, ci),
      *SchedulerFeature::SCHEDULER);
}

LeaseManager& LeaseManagerFeature::leaseManager() {
  if (!_leaseManager) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
  return *_leaseManager;
}