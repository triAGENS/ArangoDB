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

#pragma once

#include "Cluster/ClusterTypes.h"
#include "Cluster/LeaseManager/LeaseId.h"

namespace arangodb {

namespace futures {
template<typename T>
class Future;
}

namespace network {
class ConnectionPool;
}  // namespace network

class ClusterInfo;

namespace cluster {
struct ManyServersLeasesReport;

struct ILeaseManagerNetworkHandler {
  virtual ~ILeaseManagerNetworkHandler() = default;

  virtual auto abortIds(ServerID const& server,
                        std::vector<LeaseId> const& leasedFrom,
                        std::vector<LeaseId> const& leasedTo) const noexcept
      -> futures::Future<Result> = 0;

  virtual auto collectFullLeaseReport() const noexcept
      -> futures::Future<ManyServersLeasesReport> = 0;

  virtual auto collectLeaseReportForServer(ServerID const& onlyShowServer)
      const noexcept -> futures::Future<ManyServersLeasesReport> = 0;
};

struct LeaseManagerNetworkHandler : ILeaseManagerNetworkHandler {
  LeaseManagerNetworkHandler(network::ConnectionPool* pool, ClusterInfo& ci);

  auto abortIds(ServerID const& server, std::vector<LeaseId> const& leasedFrom,
                std::vector<LeaseId> const& leasedTo) const noexcept
      -> futures::Future<Result> override;

  auto collectFullLeaseReport() const noexcept
      -> futures::Future<ManyServersLeasesReport> override;

  auto collectLeaseReportForServer(ServerID const& onlyShowServer)
      const noexcept -> futures::Future<ManyServersLeasesReport> override;

 private:
  network::ConnectionPool* _pool;
  ClusterInfo& _clusterInfo;
};
}  // namespace cluster
}  // namespace arangodb