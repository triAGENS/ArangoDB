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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ApplicationFeatures/ApplicationServer.h"
#include "RestAqlFunctionsHandler.h"
#include "Aql/AqlFunctionFeature.h"

#include <velocypack/Builder.h>

using namespace arangodb;
using namespace arangodb::rest;

RestAqlFunctionsHandler::RestAqlFunctionsHandler(ArangodServer& server,
                                                 GeneralRequest* request,
                                                 GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestStatus RestAqlFunctionsHandler::execute() {
  // extract the sub-request type
  auto const type = _request->requestType();

  if (type == rest::RequestType::GET) {
    VPackBuilder builder;

    builder.openObject();
    builder.add(VPackValue("functions"));
    auto& functions = server().getFeature<aql::AqlFunctionFeature>();
    functions.toVelocyPack(builder);
    builder.close();

    generateResult(rest::ResponseCode::OK, builder.slice());
    return RestStatus::DONE;
  }

  generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  return RestStatus::DONE;
}
