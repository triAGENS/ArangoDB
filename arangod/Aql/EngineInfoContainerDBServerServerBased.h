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

#pragma once

#include "Aql/QuerySnippet.h"
#include "Aql/ShardLocking.h"
#include "Aql/types.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterTypes.h"

#include <map>
#include <stack>

namespace arangodb {

template<typename>
struct async;

namespace network {
class ConnectionPool;
struct RequestOptions;
struct Response;
}  // namespace network

namespace velocypack {
class Builder;
}

namespace aql {

class ExecutionNode;
class GatherNode;
class GraphNode;
class QueryContext;
class QuerySnippet;

class EngineInfoContainerDBServerServerBased {
 public:
  explicit EngineInfoContainerDBServerServerBased(QueryContext& query) noexcept;

  // Insert a new node into the last engine on the stack
  // If this Node contains Collections, they will be added into the map
  // for ShardLocking
  void addNode(ExecutionNode* node, bool pushToSingleServer);

  // Open a new snippet, this snippt will be used to produce data
  // for the given sinkNode (RemoteNode only for now)
  void openSnippet(GatherNode const* sinkGatherNode,
                   ExecutionNodeId idOfSinkNode);

  // Closes the given snippet and let it use
  // the given queryid of the coordinator as data provider.
  void closeSnippet(QueryId inputSnippet);

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
  //   simon: in v3.7 we get a global QueryId for all snippets on a server
  async<Result> buildEngines(
      std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
      MapRemoteToSnippet& snippetIds, aql::ServerQueryIdList& serverQueryIds,
      std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases);

  // Insert a GraphNode that needs to generate TraverserEngines on
  // the DBServers. The GraphNode itself will retain on the coordinator.
  void addGraphNode(GraphNode* node, bool pushToSingleServer);

 private:
  struct BuildEnginesInternalResult {
    Result result;
    // cleanup will be executed iff either cleanupReason is set, or if an
    // exception is thrown.
    std::optional<ErrorCode> cleanupReason;
  };
  auto buildEnginesInternal(
      std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
      MapRemoteToSnippet& snippetIds, ServerQueryIdList& serverToQueryId,
      std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases,
      std::vector<ServerID>& dbServers) -> async<BuildEnginesInternalResult>;
  /**
   * @brief Helper method to generate the Request to be send to a specific
   * database server. this request contains all the necessary information to
   * create a transaction with correct shard locking, as well as QuerySnippets
   * and GraphEngines on the receiver side.
   *
   * @param clusterQueryId cluster-wide query id (used from 3.8 onwards)
   * @param infoBuilder (mutable) the request body will be written into this
   * builder.
   * @param server The DatabaseServer we suppose to send the request to, used to
   * identify the shards on this server
   * @param nodesById A vector to get Nodes by their id.
   * @param nodeAliases (mutable) A map of node-aliases, if a server is
   * responsible for more then one shard we need to duplicate some nodes in the
   * query (e.g. an IndexNode can only access one shard at a time) this list can
   * map cloned node -> original node ids.
   *
   * @return A vector with one entry per GraphNode in the query (in order) it
   * indicates if this Server has created a GraphEngine for this Node and needs
   * to participate in the GraphOperation or not.
   */
  std::vector<bool> buildEngineInfo(
      QueryId clusterQueryId, VPackBuilder& infoBuilder, ServerID const& server,
      std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
      std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases);

  arangodb::futures::Future<Result> buildSetupRequest(
      transaction::Methods& trx, ServerID const& server, VPackSlice infoSlice,
      std::vector<bool> didCreateEngine, MapRemoteToSnippet& snippetIds,
      aql::ServerQueryIdList& serverToQueryId, std::mutex& serverToQueryIdLock,
      network::ConnectionPool* pool, network::RequestOptions const& options,
      bool fastPath) const;

  [[nodiscard]] bool isNotSatelliteLeader(VPackSlice infoSlice) const;

  /**
   * @brief Will send a shutdown to all engines registered in the list of
   * queryIds.
   * NOTE: This function will ignore all queryids where the key is not of
   * the expected format
   * they may be leftovers from Coordinator.
   * Will also clear the list of queryIds after return.
   *
   * @param errorCode error Code to be send to DBServers for logging.
   * @param dbname Name of the database this query is executed in.
   * @param serverQueryIds A map of QueryIds of the format:
   * (remoteNodeId:shardId)
   * -> queryid.
   */
  std::vector<futures::Future<network::Response>> cleanupEngines(
      ErrorCode errorCode, std::string const& dbname,
      aql::ServerQueryIdList& serverQueryIds) const;

  // Insert the Locking information into the message to be send to DBServers
  void addLockingPart(arangodb::velocypack::Builder& builder,
                      ServerID const& server) const;

  // Insert the Options information into the message to be send to DBServers
  void addOptionsPart(arangodb::velocypack::Builder& builder,
                      ServerID const& server) const;

  // Insert the Variables information into the message to be send to DBServers
  void addVariablesPart(arangodb::velocypack::Builder& builder) const;

  // Insert the Snippets information into the message to be send to DBServers
  void addSnippetPart(
      std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
      arangodb::velocypack::Builder& builder, ShardLocking& shardLocking,
      std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases,
      ServerID const& server) const;

  // Insert the TraversalEngine information into the message to be send to
  // DBServers
  std::vector<bool> addTraversalEnginesPart(
      arangodb::velocypack::Builder& builder,
      containers::FlatHashMap<ShardID, ServerID> const& shardMapping,
      ServerID const& server) const;

  // Parse the response of a DBServer to a setup request
  Result parseResponse(VPackSlice response, MapRemoteToSnippet& queryIds,
                       ServerID const& server,
                       std::vector<bool> const& didCreateEngine,
                       QueryId& globalQueryId, RebootId& rebootId,
                       bool fastPath) const;

 private:
  std::stack<std::shared_ptr<QuerySnippet>,
             std::vector<std::shared_ptr<QuerySnippet>>>
      _snippetStack;

  std::vector<std::shared_ptr<QuerySnippet>> _closedSnippets;

  QueryContext& _query;

  // @brief List of all graphNodes that need to create TraverserEngines on
  // DBServers
  std::vector<GraphNode*> _graphNodes;

  ShardLocking _shardLocking;

  // @brief A local counter for snippet IDs within this Engine.
  // used to make collection locking clear.
  QuerySnippet::Id _lastSnippetId;
};

}  // namespace aql
}  // namespace arangodb
