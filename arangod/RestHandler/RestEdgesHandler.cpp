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

#include "RestEdgesHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AstNode.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Aql/Variable.h"
#include "Basics/StringUtils.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestEdgesHandler::RestEdgesHandler(ArangodServer& server,
                                   GeneralRequest* request,
                                   GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestStatus RestEdgesHandler::execute() {
  // extract the sub-request type
  auto const type = _request->requestType();

  if (!ServerState::instance()->isSingleServerOrCoordinator()) {
    generateNotImplemented("ILLEGAL " + EDGES_PATH);
    return RestStatus::DONE;
  }

  // execute one of the CRUD methods
  switch (type) {
    case rest::RequestType::GET:
      readEdges();
      break;
    case rest::RequestType::POST:
      readEdgesForMultipleVertices();
      break;
    default:
      generateNotImplemented("ILLEGAL " + EDGES_PATH);
      break;
  }

  // this handler is done
  return RestStatus::DONE;
}

bool RestEdgesHandler::parseDirection(TRI_edge_direction_e& direction) {
  std::string const& dir = _request->value("direction");

  if (dir.empty()) {
    direction = TRI_EDGE_ANY;
  } else if (dir == "any") {
    direction = TRI_EDGE_ANY;
  } else if (dir == "out" || dir == "outbound") {
    direction = TRI_EDGE_OUT;
  } else if (dir == "in" || dir == "inbound") {
    direction = TRI_EDGE_IN;
  } else {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "<direction> must by any, in, or out, not: " + dir);
    return false;
  }

  return true;
}

namespace {
std::string queryString(TRI_edge_direction_e dir) {
  switch (dir) {
    case TRI_EDGE_IN:
      return "FOR e IN @@collection FILTER e._to == @vertex RETURN e";
    case TRI_EDGE_OUT:
      return "FOR e IN @@collection FILTER e._from == @vertex RETURN e";
    case TRI_EDGE_ANY:
      return "FOR e IN @@collection FILTER (e._from == @vertex || e._to == "
             "@vertex) RETURN e";
  }
  return "RETURN {}";
}

aql::QueryResult queryEdges(TRI_vocbase_t& vocbase, std::string const& cname,
                            TRI_edge_direction_e dir,
                            std::string const& vertexId, bool allowDirtyReads) {
  auto bindParameters = std::make_shared<VPackBuilder>();
  bindParameters->openObject();
  bindParameters->add("@collection", VPackValue(cname));
  bindParameters->add("vertex", VPackValue(vertexId));
  bindParameters->close();

  auto origin = transaction::OperationOriginREST{"querying connected edges"};
  auto ctx = transaction::StandaloneContext::create(vocbase, origin);
  aql::QueryOptions options;
  if (allowDirtyReads) {
    options.transactionOptions.allowDirtyReads = true;
  }
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      vocbase, origin, aql::QueryString(queryString(dir)),
      std::move(bindParameters), std::move(options));
  return std::move(queryFuture.waitAndGet());
}
}  // namespace

bool RestEdgesHandler::validateCollection(std::string const& name) {
  CollectionNameResolver resolver(_vocbase);
  std::shared_ptr<LogicalCollection> collection = resolver.getCollection(name);

  if (!collection) {
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return false;
  }

  if (collection->type() != TRI_COL_TYPE_EDGE) {
    generateError(rest::ResponseCode::BAD,
                  TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID);
    return false;
  }

  return true;
}

// read in- or outbound edges
bool RestEdgesHandler::readEdges() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected GET " + EDGES_PATH +
                      "/<collection-identifier>?vertex=<vertex-id>&"
                      "direction=<direction>");
    return false;
  }

  std::string const& collectionName = suffixes[0];
  if (!validateCollection(collectionName)) {
    return false;
  }

  TRI_edge_direction_e direction;
  if (!parseDirection(direction)) {
    return false;
  }

  bool found;
  std::string const& startVertex = _request->value("vertex", found);

  if (!found || startVertex.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD,
                  "illegal document handle");
    return false;
  }

  // Check if dirty reads are allowed:
  found = false;
  std::string const& val =
      _request->header(StaticStrings::AllowDirtyReads, found);
  bool allowDirtyReads = false;
  if (found && StringUtils::boolean(val)) {
    // This will be used in `createTransaction` below, if that creates
    // a new transaction. Otherwise, we use the default given by the
    // existing transaction.
    allowDirtyReads = true;
  }

  auto queryResult = ::queryEdges(_vocbase, collectionName, direction,
                                  startVertex, allowDirtyReads);
  if (queryResult.allowDirtyReads) {
    // Note that this is not necessarily the same as `allowDirtyReads` above!
    // This is, because this particular query could be in the context of a
    // transcation doing dirty reads!
    setOutgoingDirtyReadsHeader(true);
  }
  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
      generateError(rest::ResponseCode::GONE, TRI_ERROR_REQUEST_CANCELED);
      return false;
    }
    THROW_ARANGO_EXCEPTION_MESSAGE(
        queryResult.result.errorNumber(),
        StringUtils::concatT("Error executing edges query ",
                             queryResult.result.errorMessage()));
  }

  VPackSlice edges = queryResult.data->slice();
  VPackSlice extras = queryResult.extra->slice();

  VPackBuffer<uint8_t> buffer;
  VPackBuilder resultBuilder(buffer);
  resultBuilder.openObject();
  // build edges
  resultBuilder.add(VPackValue("edges"));  // only key
  resultBuilder.add(edges);

  resultBuilder.add(StaticStrings::Error, VPackValue(false));
  resultBuilder.add(StaticStrings::Code, VPackValue(200));
  VPackSlice stats = extras.get("stats");
  if (stats.isObject()) {
    resultBuilder.add(VPackValue("stats"));
    resultBuilder.add(stats);
  }
  resultBuilder.close();

  // and generate a response
  generateResult(rest::ResponseCode::OK, std::move(buffer),
                 queryResult.context);

  return true;
}

// Internal function to receive all edges for a list of vertices
// Not publicly documented on purpose.
// NOTE: It ONLY except _id strings. Nothing else
bool RestEdgesHandler::readEdgesForMultipleVertices() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected POST " + EDGES_PATH +
                      "/<collection-identifier>?direction=<direction>");
    return false;
  }

  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);

  if (!parseSuccess) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected POST " + EDGES_PATH +
                      "/<collection-identifier>?direction=<direction>");
    // A body is required
    return false;
  }

  if (!body.isArray()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "Expected an array of vertex _id's in body parameter");
    return false;
  }

  std::string const& collectionName = suffixes[0];
  if (!validateCollection(collectionName)) {
    return false;
  }

  TRI_edge_direction_e direction;
  if (!parseDirection(direction)) {
    return false;
  }

  VPackBuffer<uint8_t> buffer;
  VPackBuilder resultBuilder(buffer);
  resultBuilder.openObject();
  // build edges
  resultBuilder.add("edges", VPackValue(VPackValueType::Array));

  std::unordered_set<std::string> foundEdges;

  // Check if dirty reads are allowed:
  bool found = false;
  std::string const& val =
      _request->header(StaticStrings::AllowDirtyReads, found);
  bool allowDirtyReads = false;
  if (found && StringUtils::boolean(val)) {
    // This will be used in `createTransaction` below, if that creates
    // a new transaction. Otherwise, we use the default given by the
    // existing transaction.
    allowDirtyReads = true;
  }

  std::shared_ptr<transaction::Context> ctx;
  for (VPackSlice it : VPackArrayIterator(body)) {
    std::string startVertex = it.copyString();
    auto queryResult = ::queryEdges(_vocbase, collectionName, direction,
                                    startVertex, allowDirtyReads);

    if (queryResult.result.fail()) {
      if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
          (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
        generateError(rest::ResponseCode::GONE, TRI_ERROR_REQUEST_CANCELED);
        return false;
      }
      THROW_ARANGO_EXCEPTION_MESSAGE(
          queryResult.result.errorNumber(),
          StringUtils::concatT("Error executing edges query ",
                               queryResult.result.errorMessage()));
    }

    VPackSlice edges = queryResult.data->slice();
    for (VPackSlice edge : VPackArrayIterator(edges)) {
      if (foundEdges
              .emplace(transaction::helpers::extractKeyFromDocument(edge)
                           .copyString())
              .second) {
        resultBuilder.add(edge);
      }
    }
    ctx = queryResult.context;
  }
  resultBuilder.close();  // </edges>

  resultBuilder.add(StaticStrings::Error, VPackValue(false));
  resultBuilder.add(StaticStrings::Code, VPackValue(200));
  resultBuilder.close();

  // and generate a response
  if (ctx) {
    generateResult(rest::ResponseCode::OK, std::move(buffer), ctx);
  } else {
    generateResult(rest::ResponseCode::OK, std::move(buffer));
  }

  return true;
}
