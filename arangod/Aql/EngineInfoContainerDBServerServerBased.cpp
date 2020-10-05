////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "EngineInfoContainerDBServerServerBased.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlItemBlockSerializationFormat.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionNode.h"
#include "Aql/GraphNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/Query.h"
#include "Aql/QuerySnippet.h"
#include "Basics/StringUtils.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterTrxMethods.h"
#include "Graph/BaseOptions.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "StorageEngine/TransactionState.h"
#include "Utils/CollectionNameResolver.h"

#include <set>

using namespace arangodb;
using namespace arangodb::aql;

namespace {
const double SETUP_TIMEOUT = 90.0;

Result ExtractRemoteAndShard(VPackSlice keySlice, ExecutionNodeId& remoteId, std::string& shardId) {
  TRI_ASSERT(keySlice.isString());  // used as  a key in Json
  arangodb::velocypack::StringRef key(keySlice);
  size_t p = key.find(':');
  if (p == std::string::npos) {
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unexpected response from DBServer during setup"};
  }
  arangodb::velocypack::StringRef remId = key.substr(0, p);
  remoteId = ExecutionNodeId{basics::StringUtils::uint64(remId.begin(), remId.length())};
  if (remoteId == ExecutionNodeId{0}) {
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unexpected response from DBServer during setup"};
  }
  shardId = key.substr(p + 1).toString();
  if (shardId.empty()) {
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unexpected response from DBServer during setup"};
  }
  return {TRI_ERROR_NO_ERROR};
}

}  // namespace

EngineInfoContainerDBServerServerBased::TraverserEngineShardLists::TraverserEngineShardLists(
    GraphNode const* node, ServerID const& server,
    std::unordered_map<ShardID, ServerID> const& shardMapping, QueryContext& query)
    : _node(node), _hasShard(false) {
  auto const& edges = _node->edgeColls();
  TRI_ASSERT(!edges.empty());
  auto const& restrictToShards = query.queryOptions().restrictToShards;
  // Extract the local shards for edge collections.
  for (auto const& col : edges) {
#ifdef USE_ENTERPRISE
    if (query.trxForOptimization().isInaccessibleCollection(col->id())) {
      _inaccessible.insert(col->name());
      _inaccessible.insert(std::to_string(col->id().id()));
    }
#endif
    _edgeCollections.emplace_back(
        getAllLocalShards(shardMapping, server, col->shardIds(restrictToShards)));
  }
  // Extract vertices
  auto const& vertices = _node->vertexColls();
  // Guaranteed by addGraphNode, this will inject vertex collections
  // in anonymous graph case
  // It might in fact be empty, if we only have edge collections in a graph.
  // Or if we guarantee to never read vertex data.
  for (auto const& col : vertices) {
#ifdef USE_ENTERPRISE
    if (query.trxForOptimization().isInaccessibleCollection(col->id())) {
      _inaccessible.insert(col->name());
      _inaccessible.insert(std::to_string(col->id().id()));
    }
#endif
    auto shards = getAllLocalShards(shardMapping, server, col->shardIds(restrictToShards));
    _vertexCollections.try_emplace(col->name(), std::move(shards));
  }
}

std::vector<ShardID> EngineInfoContainerDBServerServerBased::TraverserEngineShardLists::getAllLocalShards(
    std::unordered_map<ShardID, ServerID> const& shardMapping,
    ServerID const& server, std::shared_ptr<std::vector<std::string>> shardIds) {
  std::vector<ShardID> localShards;
  for (auto const& shard : *shardIds) {
    auto const& it = shardMapping.find(shard);
    TRI_ASSERT(it != shardMapping.end());
    if (it->second == server) {
      localShards.emplace_back(shard);
      _hasShard = true;
    }
  }
  return localShards;
}

void EngineInfoContainerDBServerServerBased::TraverserEngineShardLists::serializeIntoBuilder(
    VPackBuilder& infoBuilder) const {
  TRI_ASSERT(_hasShard);
  TRI_ASSERT(infoBuilder.isOpenArray());
  infoBuilder.openObject();
  {
    // Options
    infoBuilder.add(VPackValue("options"));
    graph::BaseOptions* opts = _node->options();
    opts->buildEngineInfo(infoBuilder);
  }
  {
    // Variables
    std::vector<aql::Variable const*> vars;
    _node->getConditionVariables(vars);
    if (!vars.empty()) {
      infoBuilder.add(VPackValue("variables"));
      infoBuilder.openArray();
      for (auto v : vars) {
        v->toVelocyPack(infoBuilder);
      }
      infoBuilder.close();
    }
  }

  infoBuilder.add(VPackValue("shards"));
  infoBuilder.openObject();
  infoBuilder.add(VPackValue("vertices"));
  infoBuilder.openObject();
  for (auto const& col : _vertexCollections) {
    infoBuilder.add(VPackValue(col.first));
    infoBuilder.openArray();
    for (auto const& v : col.second) {
      infoBuilder.add(VPackValue(v));
    }
    infoBuilder.close();  // this collection
  }
  infoBuilder.close();  // vertices

  infoBuilder.add(VPackValue("edges"));
  infoBuilder.openArray();
  for (auto const& edgeShards : _edgeCollections) {
    infoBuilder.openArray();
    for (auto const& e : edgeShards) {
      infoBuilder.add(VPackValue(e));
    }
    infoBuilder.close();
  }
  infoBuilder.close();  // edges
  infoBuilder.close();  // shards

  _node->enhanceEngineInfo(infoBuilder);

  infoBuilder.close();  // base
  TRI_ASSERT(infoBuilder.isOpenArray());
}

EngineInfoContainerDBServerServerBased::EngineInfoContainerDBServerServerBased(QueryContext& query) noexcept
    : _query(query), _shardLocking(query), _lastSnippetId(1) {
  // NOTE: We need to start with _lastSnippetID > 0. 0 is reserved for GraphNodes
}

void EngineInfoContainerDBServerServerBased::injectVertexCollections(GraphNode* graphNode) {
  auto const& vCols = graphNode->vertexColls();
  if (vCols.empty()) {
    auto& resolver = _query.resolver();
    _query.collections().visit([&resolver, graphNode](std::string const& name, aql::Collection& collection) {
      // If resolver cannot resolve this collection
      // it has to be a view.
      if (resolver.getCollection(name)) {
        // All known edge collections will be ignored by this call!
        graphNode->injectVertexCollection(collection);
      }
      return true;
    });
  }
}

// Insert a new node into the last engine on the stack
// If this Node contains Collections, they will be added into the map
// for ShardLocking
void EngineInfoContainerDBServerServerBased::addNode(ExecutionNode* node, bool pushToSingleServer) {
  TRI_ASSERT(node);
  TRI_ASSERT(!_snippetStack.empty());

  // Add the node to the open Snippet
  _snippetStack.top()->addNode(node);

  switch (node->getType()) {
    case ExecutionNode::TRAVERSAL:
    case ExecutionNode::SHORTEST_PATH:
    case ExecutionNode::K_SHORTEST_PATHS: {
      auto* const graphNode = ExecutionNode::castTo<GraphNode*>(node);
      graphNode->prepareOptions();
      injectVertexCollections(graphNode);
      break;
    }
    default:
      break;
  }

  // Upgrade CollectionLocks if necessary
  _shardLocking.addNode(node, _snippetStack.top()->id(), pushToSingleServer);
}

// Open a new snippet, which provides data for the given sink node (for now only RemoteNode allowed)
void EngineInfoContainerDBServerServerBased::openSnippet(GatherNode const* sinkGatherNode,
                                                         ExecutionNodeId sinkRemoteId) {
  _snippetStack.emplace(std::make_shared<QuerySnippet>(sinkGatherNode, sinkRemoteId,
                                                       _lastSnippetId++));
}

// Closes the given snippet and connects it
// to the given queryid of the coordinator.
void EngineInfoContainerDBServerServerBased::closeSnippet(QueryId inputSnippet) {
  TRI_ASSERT(!_snippetStack.empty());
  std::shared_ptr<QuerySnippet> e = _snippetStack.top();
  TRI_ASSERT(e);
  _snippetStack.pop();
  e->useQueryIdAsInput(inputSnippet);
  _closedSnippets.emplace_back(std::move(e));
}

// Build the Engines for the DBServer
//   * Creates one Query-Entry for each Snippet per Shard (multiple on the
//   same DB)
//   * All snippets know all locking information for the query.
//   * Only the first snippet is responsible to lock.
//   * After this step DBServer-Collections are locked!
//
//   Error Case: It is guaranteed that for all snippets created during
//   this methods a shutdown request is send to all DBServers.
//   In case the network is broken and this shutdown request is lost
//   the DBServers will clean up their snippets after a TTL.
Result EngineInfoContainerDBServerServerBased::buildEngines(
    std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
    MapRemoteToSnippet& snippetIds, aql::ServerQueryIdList& serverToQueryId,
    std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases) {
  // This needs to be a set with a defined order, it is important, that we contact
  // the database servers only in this specific order to avoid cluster-wide deadlock situations.
  std::vector<ServerID> dbServers = _shardLocking.getRelevantServers();
  if (dbServers.empty()) {
    // No snippets to be placed on dbservers
    return TRI_ERROR_NO_ERROR;
  }
  // We at least have one Snippet, or one graph node.
  // Otherwise the locking needs to be empty.
  TRI_ASSERT(!_closedSnippets.empty() || !_graphNodes.empty());

  auto cleanupGuard = scopeGuard([this, &snippetIds]() {
    cleanupEngines(TRI_ERROR_INTERNAL, _query.vocbase().name(), snippetIds);
  });
  
  NetworkFeature const& nf = _query.vocbase().server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (pool == nullptr) {
    // nullptr only happens on controlled shutdown
    return {TRI_ERROR_SHUTTING_DOWN};
  }

  // Build Lookup Infos
  VPackBuilder infoBuilder;
  transaction::Methods& trx = _query.trxForOptimization();

  network::RequestOptions options;
  options.database = _query.vocbase().name();
  options.timeout = network::Timeout(SETUP_TIMEOUT);
  options.skipScheduler = true;  // hack to speed up future.get()
  options.param("ttl", std::to_string(_query.queryOptions().ttl));

  for (ServerID const& server : dbServers) {
    std::string serverDest = "server:" + server;

    LOG_TOPIC("4bbe6", DEBUG, arangodb::Logger::AQL)
        << "Building Engine Info for " << server;
    infoBuilder.clear();
    infoBuilder.openObject();
    addLockingPart(infoBuilder, server);
    TRI_ASSERT(infoBuilder.isOpenObject());

    addOptionsPart(infoBuilder, server);
    TRI_ASSERT(infoBuilder.isOpenObject());

    addVariablesPart(infoBuilder);
    TRI_ASSERT(infoBuilder.isOpenObject());
    
    infoBuilder.add("isModificationQuery", VPackValue(_query.isModificationQuery()));
    infoBuilder.add("isAsyncQuery", VPackValue(_query.isAsyncQuery()));

    infoBuilder.add(StaticStrings::AttrCoordinatorRebootId, VPackValue(ServerState::instance()->getRebootId().value()));
    infoBuilder.add(StaticStrings::AttrCoordinatorId, VPackValue(ServerState::instance()->getId()));

    addSnippetPart(nodesById, infoBuilder, _shardLocking, nodeAliases, server);
    TRI_ASSERT(infoBuilder.isOpenObject());
    auto shardMapping = _shardLocking.getShardMapping();
    std::vector<bool> didCreateEngine =
        addTraversalEnginesPart(infoBuilder, shardMapping, server);
    TRI_ASSERT(didCreateEngine.size() == _graphNodes.size());
    TRI_ASSERT(infoBuilder.isOpenObject());

    infoBuilder.add(StaticStrings::SerializationFormat,
                    VPackValue(static_cast<SerializationFormatType>(
                        aql::SerializationFormat::SHADOWROWS)));

    trx.state()->analyzersRevision().toVelocyPack(infoBuilder);
    infoBuilder.close();  // Base object
    TRI_ASSERT(infoBuilder.isClosed());

    VPackSlice infoSlice = infoBuilder.slice();
    // Partial assertions to check if all required keys are present
    TRI_ASSERT(infoSlice.hasKey("lockInfo"));
    TRI_ASSERT(infoSlice.hasKey("options"));
    TRI_ASSERT(infoSlice.hasKey("variables"));
    // We need to have at least one: snippets (non empty) or traverserEngines

    if (!((infoSlice.hasKey("snippets") && infoSlice.get("snippets").isObject() &&
           !infoSlice.get("snippets").isEmptyObject()) ||
          infoSlice.hasKey("traverserEngines"))) {
      // This is possible in the satellite case.
      // The leader of a read-only satellite is potentially
      // not part of the query.
      continue;
    }
    TRI_ASSERT((infoSlice.hasKey("snippets") && infoSlice.get("snippets").isObject() &&
                !infoSlice.get("snippets").isEmptyObject()) ||
               infoSlice.hasKey("traverserEngines"));

    VPackBuffer<uint8_t> buffer(infoSlice.byteSize());
    buffer.append(infoSlice.begin(), infoSlice.byteSize());

    // add the transaction ID header
    network::Headers headers;
    ClusterTrxMethods::addAQLTransactionHeader(trx, server, headers);
    auto res = network::sendRequest(pool, serverDest, fuerte::RestVerb::Post,
                                    "/_api/aql/setup", std::move(buffer),
                                    options, std::move(headers))
                   .get();
    _query.incHttpRequests(unsigned(1));
    if (res.fail()) {
      int code = network::fuerteToArangoErrorCode(res);
      std::string message = network::fuerteToArangoErrorMessage(res);
      LOG_TOPIC("f9a77", DEBUG, Logger::AQL)
          << server << " responded with " << code << " -> " << message;
      LOG_TOPIC("41082", TRACE, Logger::AQL) << infoSlice.toJson();
      return {code, message};
    }

    VPackSlice response = res.response->slice();
    if (response.isNone()) {
      return {TRI_ERROR_INTERNAL, "malformed response while building engines"};
    }
    QueryId globalId = 0;
    auto result = parseResponse(response, snippetIds, server, serverDest,
                                didCreateEngine, globalId);
    if (!result.ok()) {
      return result;
    }
    serverToQueryId.emplace_back(std::move(serverDest), globalId);
  }
  cleanupGuard.cancel();
  return TRI_ERROR_NO_ERROR;
}

Result EngineInfoContainerDBServerServerBased::parseResponse(
    VPackSlice response, MapRemoteToSnippet& queryIds, ServerID const& server,
    std::string const& serverDest, std::vector<bool> const& didCreateEngine,
    QueryId& globalQueryId) const {
  if (!response.isObject() || !response.get("result").isObject()) {
    LOG_TOPIC("0c3f2", ERR, Logger::AQL) << "Received error information from "
                                         << server << " : " << response.toJson();
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unable to deploy query on all required "
            "servers: " + response.toJson() + ". This can happen during "
            "failover. Please check: " +
                server};
  }

  VPackSlice result = response.get("result");
  
  // simon: in 3.7 we get a queryId for all snippets
  VPackSlice queryIdSlice = result.get("queryId");
  if (queryIdSlice.isNumber()) {
    globalQueryId = queryIdSlice.getNumber<QueryId>();
  } else {
    globalQueryId = 0;
  }
  
  VPackSlice snippets = result.get("snippets");
  // Link Snippets to their sinks
  for (auto const& resEntry : VPackObjectIterator(snippets)) {
    if (!resEntry.value.isString()) {
      return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
              "Unable to deploy query snippets on all required "
              "servers. This can happen during "
              "failover. Please check: " +
                  server};
    }
    auto remoteId = ExecutionNodeId{0};
    std::string shardId = "";
    auto res = ExtractRemoteAndShard(resEntry.key, remoteId, shardId);
    if (!res.ok()) {
      return res;
    }
    TRI_ASSERT(remoteId != ExecutionNodeId{0});
    TRI_ASSERT(!shardId.empty());
    auto& remote = queryIds[remoteId];
    auto& thisServer = remote[serverDest];
    thisServer.emplace_back(resEntry.value.copyString());
  }

  // Link traverser engines to their nodes
  VPackSlice travEngines = result.get("traverserEngines");
  if (!travEngines.isNone()) {
    if (!travEngines.isArray()) {
      return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
              "Unable to deploy query traverser engines on all required "
              "servers. This can happen during "
              "failover. Please check: " +
                  server};
    }
    auto idIter = VPackArrayIterator(travEngines);
    TRI_ASSERT(_graphNodes.size() == didCreateEngine.size());
    for (size_t i = 0; i < _graphNodes.size(); ++i) {
      if (didCreateEngine[i]) {
        if (!idIter.valid()) {
          return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
                  "The DBServer was not able to create enough "
                  "traversal engines. This can happen during "
                  "failover. Please check; " +
                      server};
        }
        _graphNodes[i]->addEngine(idIter.value().getNumber<aql::EngineId>(),
                                  server);
        idIter.next();
      }
    }
    // We need to consume all traverser engines
    TRI_ASSERT(!idIter.valid());
  }
  return {TRI_ERROR_NO_ERROR};
}

/**
 * @brief Will send a shutdown to all engines registered in the list of
 * queryIds.
 * NOTE: This function will ignore all queryids where the key is not of
 * the expected format
 * they may be leftovers from Coordinator.
 * Will also clear the list of queryIds after return.
 *
 * @param pool The ConnectionPool
 * @param errorCode error Code to be send to DBServers for logging.
 * @param dbname Name of the database this query is executed in.
 * @param queryIds A map of QueryIds of the format: (remoteNodeId:shardId)
 * -> queryid.
 */
void EngineInfoContainerDBServerServerBased::cleanupEngines(
    int errorCode, std::string const& dbname,
    MapRemoteToSnippet& snippetIds) const {
  
  NetworkFeature const& nf = _query.vocbase().server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  
  network::RequestOptions options;
  options.database = dbname;
  options.timeout = network::Timeout(10.0);  // Picked arbitrarily
  options.skipScheduler = true;              // hack to speed up future.get()

  // Shutdown query snippets
  std::string url("/_api/aql/shutdown/");
  VPackBuffer<uint8_t> body;
  VPackBuilder builder(body);
  builder.openObject();
  builder.add("code", VPackValue(std::to_string(errorCode)));
  builder.close();
  for (auto const& it : snippetIds) {
    // it.first == RemoteNodeId, we don't need this
    // it.second server -> [snippets]
    for (auto const& serToSnippets : it.second) {
      auto server = serToSnippets.first;
      for (auto const& shardId : serToSnippets.second) {
        // fire and forget
        network::sendRequest(pool, server, fuerte::RestVerb::Put, url + shardId,
                             /*copy*/ body, options);
      }
      _query.incHttpRequests(static_cast<unsigned>(serToSnippets.second.size()));
    }
  }

  // Shutdown traverser engines
  url = "/_internal/traverser/";
  VPackBuffer<uint8_t> noBody;

  for (auto const& gn : _graphNodes) {
    auto allEngines = gn->engines();
    for (auto const& engine : *allEngines) {
      // fire and forget
      network::sendRequest(pool, engine.first, fuerte::RestVerb::Delete,
                           url + basics::StringUtils::itoa(engine.second), noBody, options);
    }
    _query.incHttpRequests(static_cast<unsigned>(allEngines->size()));
  }

  snippetIds.clear();
}

// Insert a GraphNode that needs to generate TraverserEngines on
// the DBServers. The GraphNode itself will retain on the coordinator.
void EngineInfoContainerDBServerServerBased::addGraphNode(GraphNode* node, bool pushToSingleServer) {
  node->prepareOptions();
  injectVertexCollections(node);
  // SnippetID does not matter on GraphNodes
  _shardLocking.addNode(node, 0, pushToSingleServer);
  _graphNodes.emplace_back(node);
}

// Insert an Upsert that needs to generate TraverserEngines on
// the DBServers. The GraphNode itself will retain on the coordinator.
void EngineInfoContainerDBServerServerBased::addUpsertNode(UpsertNode* node) {
  // SnippetID does not matter on UpsertNodes
  _shardLocking.addNode(node, 0, false);
}

// Insert the Locking information into the message to be send to DBServers
void EngineInfoContainerDBServerServerBased::addLockingPart(arangodb::velocypack::Builder& builder,
                                                            ServerID const& server) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackValue("lockInfo"));
  builder.openObject();
  _shardLocking.serializeIntoBuilder(server, builder);
  builder.close();  // lockInfo
}

// Insert the Options information into the message to be send to DBServers
void EngineInfoContainerDBServerServerBased::addOptionsPart(arangodb::velocypack::Builder& builder,
                                                            ServerID const& server) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackValue("options"));
  // toVelocyPack will open & close the "options" object
#ifdef USE_ENTERPRISE
  if (_query.queryOptions().transactionOptions.skipInaccessibleCollections) {
    aql::QueryOptions opts = _query.queryOptions();
    TRI_ASSERT(opts.transactionOptions.skipInaccessibleCollections);
    std::vector<Collection const*> used = _shardLocking.getUsedCollections();
    for (Collection const* coll : used) {
      TRI_ASSERT(coll != nullptr);
      // simon: add collection name, plan ID and shard IDs
      if (_query.trxForOptimization().isInaccessibleCollection(coll->id())) {
        for (ShardID const& sid : _shardLocking.getShardsForCollection(server, coll)) {
          opts.inaccessibleCollections.insert(sid);
        }
        opts.inaccessibleCollections.insert(std::to_string(coll->id().id()));
      }
    }
    opts.toVelocyPack(builder, true);
  } else {
    _query.queryOptions().toVelocyPack(builder, true);
  }
#else
  _query.queryOptions().toVelocyPack(builder, true);
#endif
}

// Insert the Variables information into the message to be send to DBServers
void EngineInfoContainerDBServerServerBased::addVariablesPart(arangodb::velocypack::Builder& builder) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackValue("variables"));
  // This will open and close an Object.
  _query.ast()->variables()->toVelocyPack(builder);
}

// Insert the Snippets information into the message to be send to DBServers
void EngineInfoContainerDBServerServerBased::addSnippetPart(
    std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
    arangodb::velocypack::Builder& builder, ShardLocking& shardLocking,
    std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases, ServerID const& server) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackValue("snippets"));
  builder.openObject();
  for (auto const& snippet : _closedSnippets) {
    snippet->serializeIntoBuilder(server, nodesById, shardLocking, nodeAliases, builder);
  }
  builder.close();  // snippets
}

// Insert the TraversalEngine information into the message to be send to DBServers
std::vector<bool> EngineInfoContainerDBServerServerBased::addTraversalEnginesPart(
    arangodb::velocypack::Builder& infoBuilder,
    std::unordered_map<ShardID, ServerID> const& shardMapping, ServerID const& server) const {
  std::vector<bool> result;
  if (_graphNodes.empty()) {
    return result;
  }
  result.reserve(_graphNodes.size());
  TRI_ASSERT(infoBuilder.isOpenObject());
  infoBuilder.add(VPackValue("traverserEngines"));
  infoBuilder.openArray();
  for (auto const* graphNode : _graphNodes) {
    TraverserEngineShardLists list(graphNode, server, shardMapping, _query);
    if (list.hasShard()) {
      list.serializeIntoBuilder(infoBuilder);
    }
    // If we have a shard, we expect to get a response back for this engine
    result.emplace_back(list.hasShard());
    TRI_ASSERT(infoBuilder.isOpenArray());
  }
  infoBuilder.close();  // traverserEngines
  return result;
}
