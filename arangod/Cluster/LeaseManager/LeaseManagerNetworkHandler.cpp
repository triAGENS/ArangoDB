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

#include "LeaseManagerNetworkHandler.h"

#include "Futures/Future.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/LeaseManager/AbortLeaseInformation.h"
#include "Cluster/LeaseManager/AbortLeaseInformationInspector.h"
#include "Cluster/LeaseManager/LeasesReport.h"
#include "Cluster/LeaseManager/LeasesReportInspectors.h"
#include "Cluster/ServerState.h"
#include "Network/Methods.h"
#include "Inspection/VPack.h"

#include "Logger/LogMacros.h"

using namespace arangodb;
using namespace arangodb::cluster;

namespace {

auto collectLeaseReportForPeerServer(ClusterInfo& ci,
                                     network::ConnectionPool* pool,
                                     ServerID const& onlyShowServer) noexcept
    -> futures::Future<ManyServersLeasesReport> {
  static auto path = "/_admin/leases";
  network::RequestOptions opts;
  opts.skipScheduler = true;
  // TODO: Set small timeout!
  // opts.timeout = 1s;

  if (!onlyShowServer.empty()) {
    opts.param("server", onlyShowServer);
  }
  // We will only collect leases of servers of the other type.
  auto serverList = ServerState::instance()->isCoordinator()
                        ? ci.getCurrentDBServers()
                        : ci.getCurrentCoordinators();
  std::vector<futures::Future<ManyServersLeasesReport>> futures;
  for (auto const& server : serverList) {
    auto f =
        network::sendRequest(pool, "server:" + server,
                             arangodb::fuerte::RestVerb::Get, path, {}, opts);
    futures.emplace_back(std::move(f).thenValue(
        [server = server](network::Response result) -> ManyServersLeasesReport {
          if (result.fail() || !fuerte::statusIsSuccess(result.statusCode())) {
            ManyServersLeasesReport report;
            report.serverLeases.emplace(server,
                                        Result{result.combinedResult()});
            return report;
          }
          TRI_ASSERT(result.slice().get("error").isFalse());
          TRI_ASSERT(result.slice().get("result").isObject());
          try {
            return velocypack::deserialize<ManyServersLeasesReport>(
                result.slice().get("result"));
          } catch (...) {
            // TODO Proper report of response error!
          }
          ManyServersLeasesReport report;
          report.serverLeases.emplace(
              server, Result{TRI_ERROR_BAD_PARAMETER,
                             "Failed to deserialize server response"});
          return report;
        }));
  }

  auto allResults = co_await futures::collectAll(futures);
  ManyServersLeasesReport result;
  for (auto const& res : allResults) {
    ManyServersLeasesReport const& report = res.get();
    TRI_ASSERT(report.serverLeases.size() == 1)
        << "We got more leases from a server than expected! Every server "
           "should only report for itself";
    for (auto& [server, leases] : report.serverLeases) {
      // Merge specific result to global result
      result.serverLeases.emplace(std::move(server), std::move(leases));
    }
  }
  co_return result;
}
}  // namespace

LeaseManagerNetworkHandler::LeaseManagerNetworkHandler(
    network::ConnectionPool* pool, ClusterInfo& ci)
    : _pool{pool}, _clusterInfo{ci} {}

auto LeaseManagerNetworkHandler::abortIds(
    ServerID const& server, std::vector<LeaseId> const& leasedFrom,
    std::vector<LeaseId> const& leasedTo) const noexcept
    -> futures::Future<Result> {
  static auto path = "/_admin/leases";
  VPackBufferUInt8 buffer;
  // NOTE: We need to flip leasedFrom and leasedTo in the request.
  // Our input is: This server has leasedFrom the other server.
  // For the other the server the viewpoint is, that it has leasedTo us.
  // And vice versa.
  auto info = AbortLeaseInformation{
      .server = {.serverId = ServerState::instance()->getId(),
                 .rebootId = ServerState::instance()->getRebootId()},
      .leasedFrom = leasedTo,
      .leasedTo = leasedFrom};
  {
    VPackBuilder builder(buffer);
    arangodb::velocypack::serialize(builder, info);
  }
  network::RequestOptions opts;
  opts.skipScheduler = true;
  auto f = network::sendRequest(_pool, "server:" + server,
                                arangodb::fuerte::RestVerb::Delete, path,
                                std::move(buffer), opts);

  return std::move(f).thenValue([](network::Response result) -> Result {
    if (result.fail() || !fuerte::statusIsSuccess(result.statusCode())) {
      return result.combinedResult();
    }
    TRI_ASSERT(result.slice().get("error").isFalse());
    return Result{};
  });
}

auto LeaseManagerNetworkHandler::collectFullLeaseReport() const noexcept
    -> futures::Future<ManyServersLeasesReport> {
  co_return co_await collectLeaseReportForPeerServer(_clusterInfo, _pool, "");
}

auto LeaseManagerNetworkHandler::collectLeaseReportForServer(
    ServerID const& onlyShowServer) const noexcept
    -> futures::Future<ManyServersLeasesReport> {
  co_return co_await collectLeaseReportForPeerServer(_clusterInfo, _pool,
                                                     onlyShowServer);
}