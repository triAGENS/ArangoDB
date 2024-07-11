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

#include "LeaseManagerRestHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/LeaseManager/AbortLeaseInformation.h"
#include "Cluster/LeaseManager/AbortLeaseInformationInspector.h"
#include "Cluster/LeaseManager/LeaseManager.h"
#include "Cluster/LeaseManager/LeaseManagerFeature.h"
#include "Cluster/LeaseManager/LeasesReport.h"
#include "Cluster/LeaseManager/LeasesReportInspectors.h"
#include "Inspection/VPack.h"

#include "Logger/LogMacros.h"

using namespace arangodb;
using namespace arangodb::cluster;

namespace {

auto parseGetType(std::string_view parameter)
    -> ResultT<LeaseManager::GetType> {
  if (parameter.empty() || parameter == "local") {
    return LeaseManager::GetType::LOCAL;
  } else if (parameter == "all") {
    return LeaseManager::GetType::ALL;
  } else if (parameter == "mine") {
    return LeaseManager::GetType::MINE;
  } else if (parameter == "server") {
    return LeaseManager::GetType::FOR_SERVER;
  } else {
    // Unknown type
    return Result{TRI_ERROR_BAD_PARAMETER,
                  fmt::format("Illegal mode: {}, allowed values are: 'local', "
                              "'all', 'mine', 'server' ",
                              parameter)};
  }
}
}  // namespace

LeaseManagerRestHandler::LeaseManagerRestHandler(ArangodServer& server,
                                                 GeneralRequest* request,
                                                 GeneralResponse* response,
                                                 LeaseManager* leaseManager)
    : RestBaseHandler(server, request, response),
      _leaseManager(*leaseManager) {}

RestStatus LeaseManagerRestHandler::execute() {
  switch (request()->requestType()) {
    case RequestType::GET:
      return executeGet();
    case RequestType::DELETE_REQ: {
      bool parseSuccess = false;
      VPackSlice const body = this->parseVPackBody(parseSuccess);
      if (!parseSuccess) {
        // error message generated in parseVPackBody
        generateError(Result(TRI_ERROR_BAD_PARAMETER,
                             "Failed to Parse Body, invalid JSON."));
        return RestStatus::DONE;
      }
      auto info = velocypack::deserialize<AbortLeaseInformation>(body);
      LOG_DEVEL << "Aborting leases for server: " << body.toJson();
      return executeDelete(std::move(info));
    }
    default:
      generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND);
      return RestStatus::DONE;
  }
}

RestStatus LeaseManagerRestHandler::executeGet() {
  auto getType = parseGetType(request()->value("type"));
  if (getType.fail()) {
    generateError(getType.result());
    return RestStatus::DONE;
  }
  auto forServer = std::optional<std::string>{};
  if (getType == LeaseManager::GetType::FOR_SERVER) {
    auto serverValue = request()->value("server");
    if (!serverValue.empty()) {
      forServer = std::move(serverValue);
    }
  }
  auto report = _leaseManager.reportLeases(getType.get(), forServer);
  auto builder = velocypack::serialize(report);
  generateOk(rest::ResponseCode::OK, builder.slice());
  return RestStatus::DONE;
}

RestStatus LeaseManagerRestHandler::executeDelete(AbortLeaseInformation info) {
  _leaseManager.abortLeasesForServer(std::move(info));
  // This API can only return 200, leases are guaranteed to be aborted.
  generateOk(rest::ResponseCode::OK, VPackSlice::noneSlice());
  return RestStatus::DONE;
}