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

#ifndef ARANGOD_AQL_ENGINE_INFO_CONTAINER_DBSERVER_SERVER_BASED_H
#define ARANGOD_AQL_ENGINE_INFO_CONTAINER_DBSERVER_SERVER_BASED_H 1

#include "Basics/Common.h"

#include "Aql/QuerySnippet.h"
#include "Aql/ShardLocking.h"
#include "Aql/types.h"
#include "Cluster/ClusterInfo.h"
#include "VocBase/AccessMode.h"

#include <map>
#include <set>
#include <stack>

namespace arangodb {
namespace network {
class ConnectionPool;
}

namespace velocypack {
class Builder;
}

namespace aql {

class ExecutionNode;
class GatherNode;
class GraphNode;
class UpsertNode;
class QueryContext;
class QuerySnippet;

class EngineInfoContainerDBServerServerBased {
 private:
  // @brief Local struct to create the
  // information required to build traverser engines
  // on DB servers.
  class TraverserEngineShardLists {
   public:
    TraverserEngineShardLists(GraphNode const*, ServerID const& server,
                              std::unordered_map<ShardID, ServerID> const& shardMapping,
                              QueryContext& query);

    ~TraverserEngineShardLists() = default;

    void serializeIntoBuilder(arangodb::velocypack::Builder& infoBuilder) const;

    bool hasShard() const { return _hasShard; }
    
    /// inaccessible edge and verte collection names
#ifdef USE_ENTERPRISE
    std::set<CollectionID> inaccessibleCollNames() const {
      return _inaccessible;
    }
#endif

   private:
    std::vector<ShardID> getAllLocalShards(std::unordered_map<ShardID, ServerID> const& shardMapping,
                                           ServerID const& server,
                                           std::shared_ptr<std::vector<std::string>> shardIds);

   private:
    // The graph node we need to serialize
    GraphNode const* _node;

    // Flag if we found any shard for the given server.
    // If not serializeToBuilder will be a noop
    bool _hasShard;

    // Mapping for edge collections to shardIds.
    // We have to retain the ordering of edge collections, all
    // vectors of these in one run need to have identical size.
    // This is because the conditions to query those edges have the
    // same ordering.
    std::vector<std::vector<ShardID>> _edgeCollections;

    // Mapping for vertexCollections to shardIds.
    std::unordered_map<std::string, std::vector<ShardID>> _vertexCollections;

#ifdef USE_ENTERPRISE
    std::set<CollectionID> _inaccessible;
#endif
  };

 public:
  explicit EngineInfoContainerDBServerServerBased(QueryContext& query) noexcept;

  // Insert a new node into the last engine on the stack
  // If this Node contains Collections, they will be added into the map
  // for ShardLocking
  void addNode(ExecutionNode* node, bool pushToSingleServer);

  // Open a new snippet, this snippt will be used to produce data
  // for the given sinkNode (RemoteNode only for now)
  void openSnippet(GatherNode const* sinkGatherNode, ExecutionNodeId idOfSinkNode);

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
  Result buildEngines(std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
                      MapRemoteToSnippet& snippetIds,
                      aql::ServerQueryIdList& serverQueryIds,
                      std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases);

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
  void cleanupEngines(int errorCode, std::string const& dbname,
                      MapRemoteToSnippet& queryIds) const;

  // Insert a GraphNode that needs to generate TraverserEngines on
  // the DBServers. The GraphNode itself will retain on the coordinator.
  void addGraphNode(GraphNode* node, bool pushToSingleServer);

  void addUpsertNode(UpsertNode* node);

 private:
  // Insert the Locking information into the message to be send to DBServers
  void addLockingPart(arangodb::velocypack::Builder& builder, ServerID const& server) const;

  // Insert the Options information into the message to be send to DBServers
  void addOptionsPart(arangodb::velocypack::Builder& builder, ServerID const& server) const;

  // Insert the Variables information into the message to be send to DBServers
  void addVariablesPart(arangodb::velocypack::Builder& builder) const;

  // Insert the Snippets information into the message to be send to DBServers
  void addSnippetPart(std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
                      arangodb::velocypack::Builder& builder, ShardLocking& shardLocking,
                      std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases,
                      ServerID const& server) const;

  // Insert the TraversalEngine information into the message to be send to DBServers
  std::vector<bool> addTraversalEnginesPart(arangodb::velocypack::Builder& builder,
                                            std::unordered_map<ShardID, ServerID> const& shardMapping,
                                            ServerID const& server) const;

  // Parse the response of a DBServer to a setup request
  Result parseResponse(VPackSlice response, MapRemoteToSnippet& queryIds,
                       ServerID const& server, std::string const& serverDest,
                       std::vector<bool> const& didCreateEngine,
                       QueryId& globalQueryId) const;

  void injectVertexCollections(GraphNode* node);
 private:
  std::stack<std::shared_ptr<QuerySnippet>, std::vector<std::shared_ptr<QuerySnippet>>> _snippetStack;

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
#endif
