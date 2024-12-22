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

#include "InternalRestTraverserHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryRegistry.h"
#include "Basics/ScopeGuard.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngine.h"
#include "Logger/LogMacros.h"
#include "Rest/GeneralResponse.h"
#include "Transaction/StandaloneContext.h"

#include <Logger/LogMacros.h>
#include <chrono>
#include <thread>

using namespace arangodb;
using namespace arangodb::traverser;
using namespace arangodb::rest;

InternalRestTraverserHandler::InternalRestTraverserHandler(
    ArangodServer& server, GeneralRequest* request, GeneralResponse* response,
    aql::QueryRegistry* engineRegistry)
    : RestVocbaseBaseHandler(server, request, response),
      _registry(engineRegistry) {
  TRI_ASSERT(_registry != nullptr);
}

RestStatus InternalRestTraverserHandler::execute() {
  if (!ServerState::instance()->isDBServer()) {
    generateForbidden();
    return RestStatus::DONE;
  }

  // extract the sub-request type
  auto const type = _request->requestType();

  // execute one of the CRUD methods
  try {
    switch (type) {
      case RequestType::POST:
        createEngine();
        break;
      case RequestType::PUT:
        queryEngine();
        break;
      case RequestType::DELETE_REQ:
        destroyEngine();
        break;

      default:
        generateNotImplemented("ILLEGAL " + EDGES_PATH);
        break;
    }
  } catch (arangodb::basics::Exception const& ex) {
    generateError(GeneralResponse::responseCode(ex.code()), ex.code(),
                  ex.what());
  } catch (std::exception const& ex) {
    generateError(ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    generateError(ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL);
  }

  // this handler is done
  return RestStatus::DONE;
}

void InternalRestTraverserHandler::createEngine() {
  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_NOT_IMPLEMENTED,
      "API traversal engine creation no longer supported");
}

void InternalRestTraverserHandler::queryEngine() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();
  size_t count = suffixes.size();
  if (count != 2) {
    generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected PUT " + INTERNAL_TRAVERSER_PATH +
                      "/[vertex|edge]/<TraverserEngineId>");
    return;
  }

  std::string const& option = suffixes[0];
  auto engineId =
      static_cast<aql::EngineId>(basics::StringUtils::uint64(suffixes[1]));
  if (engineId == 0) {
    generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected TraveserEngineId to be an integer number");
    return;
  }

  bool parseSuccess = true;
  VPackSlice body = this->parseVPackBody(parseSuccess);

  if (!parseSuccess) {
    generateError(
        ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
        "expecting a valid object containing the keys 'depth' and 'keys'");
    return;
  }

  std::chrono::time_point<std::chrono::steady_clock> start =
      std::chrono::steady_clock::now();

  traverser::BaseEngine* engine = nullptr;
  while (true) {
    try {
      engine = _registry->openGraphEngine(engineId);
      if (engine != nullptr) {
        break;
      }
      generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "invalid TraverserEngine id - potentially the AQL query "
                    "was already aborted or timed out");
      return;
    } catch (basics::Exception const& ex) {
      // it is possible that the engine is already in use
      if (ex.code() != TRI_ERROR_LOCKED) {
        throw;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (server().isStopping()) {
      generateError(ResponseCode::BAD, TRI_ERROR_SHUTTING_DOWN);
      return;
    }

    if (start + std::chrono::seconds(60) < std::chrono::steady_clock::now()) {
      // timeout
      generateError(ResponseCode::SERVER_ERROR, TRI_ERROR_LOCK_TIMEOUT);
      return;
    }
  }

  TRI_ASSERT(engine != nullptr);

  auto& registry = _registry;  // For the guard
  auto cleanup = scopeGuard([registry, &engineId]() noexcept {
    try {
      registry->closeEngine(engineId);
    } catch (std::exception const& ex) {
      LOG_TOPIC("dfc7a", ERR, Logger::AQL)
          << "Failed to close engine: " << ex.what();
    }
  });

  if (option == "lock") {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_NOT_IMPLEMENTED,
        "API for traversal engine locking no longer supported");
  }

  VPackBuilder result;
  if (option == "edge") {
    VPackSlice keysSlice = body.get("keys");

    if (!keysSlice.isString() && !keysSlice.isArray()) {
      generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expecting 'keys' to be a string or an array value.");
      return;
    }

    switch (engine->getType()) {
      case BaseEngine::EngineType::TRAVERSER: {
        VPackSlice depthSlice = body.get("depth");
        if (!depthSlice.isInteger()) {
          generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                        "expecting 'depth' to be an integer value");
          return;
        }
        // Safe cast BaseTraverserEngines are all of type TRAVERSER
        auto eng = static_cast<BaseTraverserEngine*>(engine);
        TRI_ASSERT(eng != nullptr);

        VPackSlice variables = body.get("variables");
        eng->injectVariables(variables);

        eng->getEdges(keysSlice, depthSlice.getNumericValue<size_t>(), result);
        break;
      }
      case BaseEngine::EngineType::SHORTESTPATH: {
        // The following option was introduced in 3.12.3 and asks to
        // return the list of edges as a list of lists, rather than a
        // flat list.
        bool listoflists = false;
        VPackSlice styleSlice = body.get("style");
        if (styleSlice.isString() && styleSlice.stringView() == "listoflists") {
          listoflists = true;
        }

        VPackSlice bwSlice = body.get("backward");
        if (!bwSlice.isBool()) {
          generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                        "expecting 'backward' to be a boolean value");
          return;
        }
        // Safe cast ShortestPathEngines are all of type SHORTESTPATH
        auto eng = static_cast<ShortestPathEngine*>(engine);
        TRI_ASSERT(eng != nullptr);
        eng->getEdges(keysSlice, bwSlice.getBoolean(), listoflists, result);
        break;
      }
      default:
        generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                      "this engine does not support the requested operation.");
        return;
    }
  } else if (option == "vertex") {
    VPackSlice keysSlice = body.get("keys");

    if (!keysSlice.isString() && !keysSlice.isArray()) {
      generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expecting 'keys' to be a string or an array value.");
      return;
    }

    VPackSlice depthSlice = body.get("depth");
    if (!depthSlice.isNone() && !depthSlice.isInteger()) {
      generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expecting 'depth' to be an integer value");
      return;
    }
    engine->getVertexData(keysSlice, result, !depthSlice.isNone());
  } else if (option == "smartSearch" || option == "smartSearchBFS" ||
             option == "smartSearchWeighted" ||
             option == "smartSearchUnified") {
    if (engine->getType() != BaseEngine::EngineType::TRAVERSER) {
      generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "this engine does not support the requested operation.");
      return;
    }
    // Safe cast BaseTraverserEngines are all of type TRAVERSER
    auto eng = static_cast<BaseTraverserEngine*>(engine);
    TRI_ASSERT(eng != nullptr);

    try {
      if (option == "smartSearchUnified") {
        eng->smartSearchUnified(body, result);
      } else {
        // TODO: Take deprecation path!
        eng->smartSearch(body, result);
      }
    } catch (arangodb::basics::Exception const& ex) {
      generateError(ResponseCode::BAD, ex.code(), ex.what());
      return;
    }
  } else {
    // PATH Info wrong other error
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND, "");
    return;
  }
  generateResult(ResponseCode::OK, result.slice(), engine->context());
}

// simon: this API will no longer be used in 3.7 to regularly
// shut down an AQL query, but it can be used during query setup if the
// setup fails.
void InternalRestTraverserHandler::destroyEngine() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();
  if (suffixes.size() != 1) {
    // DELETE requires the id as path parameter
    generateError(
        ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
        "expected DELETE " + INTERNAL_TRAVERSER_PATH + "/<TraverserEngineId>");
    return;
  }

  auto engineId =
      static_cast<aql::EngineId>(basics::StringUtils::uint64(suffixes[0]));
  bool found = _registry->destroyEngine(engineId, TRI_ERROR_NO_ERROR);
  generateResult(ResponseCode::OK, VPackSlice::booleanSlice(found));
}
