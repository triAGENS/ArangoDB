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
#include "Cluster/LeaseManager/AbortLeaseInformation.h"
#include "Cluster/LeaseManager/AbortLeaseInformationInspector.h"

using namespace arangodb::cluster;

LeaseManagerNetworkHandler::LeaseManagerNetworkHandler(
    network::ConnectionPool* pool)
    : _pool{pool} {}

auto LeaseManagerNetworkHandler::abortIds(ServerID const& server, std::vector<LeaseId> const& ids)
    const noexcept -> arangodb::futures::Future<arangodb::Result> {

  auto promise = futures::Promise<Result>{};
  auto future = promise.getFuture();
  // TODO: Implement me!
  promise.setValue(Result{});
  return future;
}
