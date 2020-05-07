////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019-2019 ArangoDB GmbH, Cologne, Germany
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

#include "QuerySnippet.h"

#include "Aql/ClusterNodes.h"
#include "Aql/Collection.h"
#include "Aql/CollectionAccessingNode.h"
#include "Aql/DistributeConsumerNode.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/GraphNode.h"
#include "Aql/IResearchViewNode.h"
#include "Aql/ShardLocking.h"
#include "Basics/StringUtils.h"
#include "Cluster/ServerState.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/Aql/LocalGraphNode.h"
#endif

#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

namespace {
DistributeConsumerNode* createConsumerNode(ExecutionPlan* plan, ScatterNode* internalScatter,
                                           std::string_view const distributeId) {
  auto uniq_consumer =
      std::make_unique<DistributeConsumerNode>(plan, plan->nextId(),
                                               std::string(distributeId));
  auto consumer = uniq_consumer.get();
  TRI_ASSERT(consumer != nullptr);
  // Hand over responsibility to plan, s.t. it can clean up if one of the below fails
  plan->registerNode(uniq_consumer.release());
  consumer->setIsInSplicedSubquery(internalScatter->isInSplicedSubquery());
  consumer->addDependency(internalScatter);
  consumer->cloneRegisterPlan(internalScatter);
  internalScatter->addClient(consumer);
  return consumer;
}

}  // namespace

using LocalExpansions = std::unordered_map<ExecutionNode*, std::set<ShardID>>;
using NodeAliasMap = std::map<ExecutionNodeId, ExecutionNodeId>;

// The CloneWorker "clones" a snippet
//
// rootNode -> N1 -> N2 -> ... -> Nk -> remoteNode
//
// to
//
// internalGather -> rootNode -> CN1 -> CN2 -> ... -> CNk -> DistributeConsumerNode -> internalScatter
//
// where CN1 ... CNk are clones of N1 ... Nk, taking into account subquery nodes.
//
// This is used to create a plan of the form
//
//                           INTERNAL_SCATTER
//            /               |                         \
//           /                |                          \
//  DistributeConsumer   DistributeConsumer  ...  DistributeConsumer
//          |                 |                           |
//         CNk               CNk                         CNk
//          |                 |                           |
//         ...               ...                         ...
//          |                 |                           |
//         CN0               CN0                         CN0
//          \                 |                           /
//           \                |                          /
//                           INTERNAL_GATHER
//

class CloneWorker final : public WalkerWorker<ExecutionNode> {
 public:
  explicit CloneWorker(ExecutionNode* rootNode, RemoteNode* remoteNode,
                       GatherNode* internalGather, ScatterNode* internalScatter,
                       LocalExpansions const& localExpansions, size_t shardId,
                       std::string_view const distId, NodeAliasMap& nodeAliases);

  bool before(ExecutionNode* node) final;
  void after(ExecutionNode* node) final;
  bool enterSubquery(ExecutionNode* subq, ExecutionNode* root) final;

 private:
  ExecutionNode* _root{nullptr};
  RemoteNode* _remote{nullptr};
  ScatterNode* _internalScatter;
  GatherNode* _internalGather;
  LocalExpansions const& _localExpansions;
  size_t _shardId{0};
  std::string_view const _distId;
  std::map<ExecutionNode*, ExecutionNode*> _originalToClone;
  NodeAliasMap& _nodeAliases;
};

CloneWorker::CloneWorker(ExecutionNode* root, RemoteNode* remote,
                         GatherNode* internalGather, ScatterNode* internalScatter,
                         LocalExpansions const& localExpansions, size_t shardId,
                         std::string_view const distId, NodeAliasMap& nodeAliases)
    : _root{root},
      _remote{remote},
      _internalScatter{internalScatter},
      _internalGather{internalGather},
      _localExpansions(localExpansions),
      _shardId(shardId),
      _distId(distId),
      _nodeAliases{nodeAliases} {}

bool CloneWorker::before(ExecutionNode* node) {
  auto plan = node->plan();

  // We don't clone the remote node, but create a DistributeConsumerNode instead
  // This will get `internalScatter` as its sole dependency
  // TODO: probably should set the map differently here
  if (node == _remote) {
    DistributeConsumerNode* consumer = createConsumerNode(plan, _internalScatter, _distId);
    consumer->isResponsibleForInitializeCursor(false);
    _nodeAliases.try_emplace(consumer->id(), std::numeric_limits<size_t>::max());
  } else if (node == _internalGather || node == _internalScatter) {
    // Never clone these nodes. We should not run into this case?
  } else {
    auto clone = node->clone(plan, false, false);

    // If the node is in _localExpansions it has to be collectionAccessing
    auto permuter = _localExpansions.find(node);
    if (permuter != _localExpansions.end()) {
      auto collectionAccessingNode = dynamic_cast<CollectionAccessingNode*>(clone);
      TRI_ASSERT(collectionAccessingNode != nullptr);
      // Get the `i` th shard
      collectionAccessingNode->setUsedShard(*std::next(permuter->second.begin(), _shardId));
    }

    TRI_ASSERT(clone->id() != node->id());
    _originalToClone.try_emplace(node, clone);
    _nodeAliases.try_emplace(clone->id(), node->id());
  }
  return true;
}

// This hooks up dependencies; we're doing this in after to make sure
// that all nodes (including those contained in subqueries!) are cloned
void CloneWorker::after(ExecutionNode* node) {
  auto clone = _originalToClone.at(node);

  auto deps = node->getDependencies();
  for (auto d : deps) {
    auto depClone = _originalToClone.at(d);
    clone->addDependency(depClone);
  }

  if (node == _root) {
    _internalGather->addDependency(clone);
  }

  // for a SubqueryNode, we need to hook up both
  // the root of the Subquery as well as the SubqueryNode
  // itself
  if (node->getType() == ExecutionNode::SUBQUERY) {
    auto sq = ExecutionNode::castTo<SubqueryNode*>(node);
    auto cloneSq = ExecutionNode::castTo<SubqueryNode*>(clone);

    auto sqRoot = sq->getSubquery();
    auto cloneSqRoot = _originalToClone.at(sqRoot);
    cloneSq->setSubquery(cloneSqRoot, true);
  }
}

bool CloneWorker::enterSubquery(ExecutionNode* subq, ExecutionNode* root) {
  return true;
}

void QuerySnippet::addNode(ExecutionNode* node) {
  TRI_ASSERT(node != nullptr);
  _nodes.push_back(node);

  switch (node->getType()) {
    case ExecutionNode::REMOTE: {
      TRI_ASSERT(_remoteNode == nullptr);
      _remoteNode = ExecutionNode::castTo<RemoteNode *>(node);
      break;
    }
    case ExecutionNode::ENUMERATE_COLLECTION:
    case ExecutionNode::INDEX:
    case ExecutionNode::INSERT:
    case ExecutionNode::UPDATE:
    case ExecutionNode::REMOVE:
    case ExecutionNode::REPLACE:
    case ExecutionNode::UPSERT: {
      // We do not actually need to know the details here.
      // We just wanna know the shards!
      auto collectionAccessingNode = dynamic_cast<CollectionAccessingNode*>(node);
      TRI_ASSERT(collectionAccessingNode != nullptr);
      _expansions.emplace_back(node, !collectionAccessingNode->isUsedAsSatellite(),
                               collectionAccessingNode->isUsedAsSatellite());
      break;
    }
    case ExecutionNode::TRAVERSAL:
    case ExecutionNode::SHORTEST_PATH:
    case ExecutionNode::K_SHORTEST_PATHS: {
      auto* graphNode = ExecutionNode::castTo<GraphNode*>(node);
      auto const isSatellite = graphNode->isUsedAsSatellite();
      _expansions.emplace_back(node, !isSatellite, isSatellite);
      break;
    }
    case ExecutionNode::ENUMERATE_IRESEARCH_VIEW: {
      auto viewNode = ExecutionNode::castTo<iresearch::IResearchViewNode*>(node);

      // evaluate node volatility before the distribution
      // can't do it on DB servers since only parts of the plan will be sent
      viewNode->volatility(true);
      _expansions.emplace_back(node, false, false);
      break;
    }
    case ExecutionNode::MATERIALIZE: {
      auto collectionAccessingNode = dynamic_cast<CollectionAccessingNode*>(node);
      // Materialize index node - true
      // Materialize view node - false
      if (collectionAccessingNode != nullptr) {
        _expansions.emplace_back(node, true, false);
      }
      break;
    }
    default:
      // do nothing
      break;
  }
}

void QuerySnippet::serializeIntoBuilder(
    ServerID const& server,
    std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
    ShardLocking& shardLocking,
    std::map<ExecutionNodeId, ExecutionNodeId>& nodeAliases,
    VPackBuilder& infoBuilder) {
  TRI_ASSERT(!_nodes.empty());
  TRI_ASSERT(!_expansions.empty());
  auto firstBranchRes = prepareFirstBranch(server, nodesById, shardLocking);
  if (!firstBranchRes.ok()) {
    // We have at least one expansion that has no shard on this server.
    // we do not need to write this snippet here.
    TRI_ASSERT(firstBranchRes.is(TRI_ERROR_CLUSTER_NOT_LEADER));
    return;
  }
  std::unordered_map<ExecutionNode*, std::set<ShardID>>& localExpansions =
      firstBranchRes.get();

  // We clone every Node* and maintain a list of ReportingGroups for profiler
  if (_remoteNode != nullptr) {
    if (!_madeResponsibleForShutdown) {
      // Enough to do this step once
      // We need to connect this Node to the sink
      // update the remote node with the information about the query
      _remoteNode->server("server:" + arangodb::ServerState::instance()->getId());
      _remoteNode->queryId(_inputSnippet);

      // A Remote can only contact a global SCATTER or GATHER node.
      TRI_ASSERT(_remoteNode->getFirstDependency() != nullptr);
      TRI_ASSERT(_remoteNode->getFirstDependency()->getType() == ExecutionNode::SCATTER ||
                 _remoteNode->getFirstDependency()->getType() == ExecutionNode::DISTRIBUTE);
      TRI_ASSERT(_remoteNode->hasDependency());

      // Need to wire up the internal scatter and distribute consumer in between the last two nodes
      // Note that we need to clean up this after we produced the snippets.
      _globalScatter = ExecutionNode::castTo<ScatterNode*>(_remoteNode->getFirstDependency());
      // Let the globalScatter node distribute data by server
      _globalScatter->setScatterType(ScatterNode::ScatterType::SERVER);

      // All of the above could be repeated as it is constant information.
      // this one line is the important one
      _remoteNode->isResponsibleForInitializeCursor(true);
      _madeResponsibleForShutdown = true;
    } else {
      _remoteNode->isResponsibleForInitializeCursor(false);
    }
    // We have it all serverbased now
    _remoteNode->setDistributeId(server);
    // Wire up this server to the global scatter
    TRI_ASSERT(_globalScatter != nullptr);
    _globalScatter->addClient(_remoteNode);

    // For serialization remove the dependency of Remote
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    std::vector<ExecutionNode*> deps;
    _remoteNode->dependencies(deps);
    TRI_ASSERT(deps.size() == 1);
    TRI_ASSERT(deps[0] == _globalScatter);
#endif
    _remoteNode->removeDependencies();
  }

  // The Key is required to build up the queryId mapping later
  infoBuilder.add(VPackValue(
      arangodb::basics::StringUtils::itoa(_idOfSinkRemoteNode.id()) + ":" + server));
  if (!localExpansions.empty()) { // one expansion
    // We have Expansions to permutate, guaranteed they have
    // all identical lengths.
    size_t numberOfShardsToPermutate = localExpansions.begin()->second.size();
    TRI_ASSERT(numberOfShardsToPermutate > 1);
    
    std::vector<std::string> distIds{};
    // Reserve the amount of localExpansions,
    distIds.reserve(numberOfShardsToPermutate);
    // Create an internal GatherNode, that will connect to all execution
    // steams of the query
    auto plan = _nodes.front()->plan();
    TRI_ASSERT(plan == _sinkNode->plan());
    // Clone the sink node, we do not need dependencies (second bool)
    // And we do not need variables
    auto* internalGather =
        ExecutionNode::castTo<GatherNode*>(_sinkNode->clone(plan, false, false));
    // Use the same elements for sorting
    internalGather->elements(_sinkNode->elements());
    // We need to modify the registerPlanning.
    // The internalGather is NOT allowed to reduce the number of registers,
    // it needs to expose it's input register by all means
    internalGather->setVarsUsedLater(_nodes.front()->getVarsUsedLater());
    internalGather->setRegsToClear({});
    auto const reservedId = ExecutionNodeId{std::numeric_limits<ExecutionNodeId::BaseType>::max()};
    nodeAliases.try_emplace(internalGather->id(), reservedId);

    ScatterNode* internalScatter = nullptr;
    if (_remoteNode != nullptr) {  // RemoteBlock talking to coordinator snippet
      TRI_ASSERT(_globalScatter != nullptr);
      TRI_ASSERT(plan == _globalScatter->plan());
      internalScatter =
          ExecutionNode::castTo<ScatterNode*>(_globalScatter->clone(plan, false, false));
      internalScatter->clearClients();

      TRI_ASSERT(_remoteNode->getDependencies().size() == 0);

      ExecutionNode* secondToLast = _remoteNode->getFirstParent();
      plan->unlinkNode(_remoteNode);

      internalScatter->addDependency(_remoteNode);

      // Let the local Scatter node distribute data by SHARD
      internalScatter->setScatterType(ScatterNode::ScatterType::SHARD);
      nodeAliases.try_emplace(internalScatter->id(), reservedId);

      if (_globalScatter->getType() == ExecutionNode::DISTRIBUTE) {
        {
          // We are not allowed to generate new keys on the DBServer.
          // We need to take the ones generated by the node above.
          DistributeNode* internalDist =
              ExecutionNode::castTo<DistributeNode*>(internalScatter);
          internalDist->setCreateKeys(false);
        }
        DistributeNode const* dist =
            ExecutionNode::castTo<DistributeNode const*>(_globalScatter);
        TRI_ASSERT(dist != nullptr);
        auto distCollection = dist->collection();
        TRI_ASSERT(distCollection != nullptr);

        // Now find the node that provides the distribute information
        for (auto const& exp : localExpansions) {
          auto colAcc = dynamic_cast<CollectionAccessingNode*>(exp.first);
          TRI_ASSERT(colAcc != nullptr);
          if (colAcc->collection() == distCollection) {
            // Found one, use all shards of it
            for (auto const& s : exp.second) {
              distIds.emplace_back(s);
            }
            break;
          }
        }
      } else {
        // In this case we actually do not care for the real value, we just need
        // to ensure that every client get's exactly one copy.
        for (size_t i = 0; i < numberOfShardsToPermutate; i++) {
          distIds.emplace_back(StringUtils::itoa(i));
        }
      }

      // hook distribute node into stream '0', since that does not happen below
      DistributeConsumerNode* consumer =
          createConsumerNode(plan, internalScatter, distIds[0]);
      nodeAliases.try_emplace(consumer->id(), std::numeric_limits<size_t>::max());
      // now wire up the temporary nodes

      TRI_ASSERT(_nodes.size() > 1);

      TRI_ASSERT(!secondToLast->hasDependency());
      secondToLast->addDependency(consumer);
    }
    
#if 0
    // hook in the async executor node, if required
    if (internalGather->parallelism() == GatherNode::Parallelism::Async) {
      TRI_ASSERT(internalScatter == nullptr);
      auto async = std::make_unique<AsyncNode>(plan, plan->nextId());
      async->addDependency(_nodes.front());
      async->setIsInSplicedSubquery(_nodes.front()->isInSplicedSubquery());
      async->cloneRegisterPlan(_nodes.front());
      _nodes.insert(_nodes.begin(), async.get());
      plan->registerNode(async.release());
    }
#endif
    
    // We do not need to copy the first stream, we can use the one we have.
    // We only need copies for the other streams.
    internalGather->addDependency(_nodes.front());

    // NOTE: We will copy over the entire snippet stream here.
    // We will inject the permuted shards on the way.
    // Also note: the local plan will take memory responsibility
    // of the ExecutionNodes created during this procedure.
    auto snippetRoot = _nodes.at(0);

    for (size_t i = 1; i < numberOfShardsToPermutate; ++i) {
      auto cloneWorker = CloneWorker(snippetRoot, _remoteNode, internalGather, internalScatter,
                                     localExpansions, i, distIds[i], nodeAliases);
      snippetRoot->walk(cloneWorker);
    }

    const unsigned flags = ExecutionNode::SERIALIZE_DETAILS;
    internalGather->toVelocyPack(infoBuilder, flags, false);

  } else {
    const unsigned flags = ExecutionNode::SERIALIZE_DETAILS;
    _nodes.front()->toVelocyPack(infoBuilder, flags, false);
  }

  if (_remoteNode != nullptr) {
    // For the local copy read the dependency of Remote
    TRI_ASSERT(_remoteNode->getDependencies().size() == 0);
    _remoteNode->addDependency(_globalScatter);
  }
}

ResultT<std::unordered_map<ExecutionNode*, std::set<ShardID>>> QuerySnippet::prepareFirstBranch(
    ServerID const& server,
    std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
    ShardLocking& shardLocking) {
  size_t numberOfShardsToPermutate = 0;
  std::unordered_map<ExecutionNode*, std::set<ShardID>> localExpansions;
  std::unordered_map<ShardID, ServerID> const& shardMapping =
      shardLocking.getShardMapping();

  for (auto const& exp : _expansions) {
    if (exp.node->getType() == ExecutionNode::ENUMERATE_IRESEARCH_VIEW) {
      // Special case, VIEWs can serve more than 1 shard per Node.
      // We need to inject them all at once.
      auto* viewNode = ExecutionNode::castTo<iresearch::IResearchViewNode*>(exp.node);
      auto& viewShardList = viewNode->shards();
      viewShardList.clear();

      auto collections = viewNode->collections();
      for (aql::Collection const& c : collections) {
        auto const& shards = shardLocking.shardsForSnippet(id(), &c);
        for (auto const& s : shards) {
          auto check = shardMapping.find(s);
          // If we find a shard here that is not in this mapping,
          // we have 1) a problem with locking before that should have thrown
          // 2) a problem with shardMapping lookup that should have thrown before
          TRI_ASSERT(check != shardMapping.end());
          if (check->second == server) {
            viewShardList.emplace_back(s);
          }
        }
      }
    } else if (exp.node->getType() == ExecutionNode::TRAVERSAL ||
               exp.node->getType() == ExecutionNode::SHORTEST_PATH ||
               exp.node->getType() == ExecutionNode::K_SHORTEST_PATHS) {
#ifndef USE_ENTERPRISE
      // These can only ever be LocalGraphNodes, which are only available in
      // Enterprise. This should never happen without enterprise optimization,
      // either.
      TRI_ASSERT(false);
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
#else
      // the same translation is copied to all servers
      // there are no local expansions

      auto* localGraphNode = ExecutionNode::castTo<LocalGraphNode*>(exp.node);
      localGraphNode->setCollectionToShard({});  // clear previous information

      TRI_ASSERT(localGraphNode->isUsedAsSatellite() == exp.isSatellite);

      // Check whether `servers` is the leader for any of the shards of the
      // prototype collection.
      // We want to instantiate this snippet here exactly iff this is the case.
      auto needInstanceHere = std::invoke([&]() {
        auto const* const protoCol = localGraphNode->isUsedAsSatellite()
                ? ExecutionNode::castTo<CollectionAccessingNode*>(
                      localGraphNode->getSatelliteOf(nodesById))
                      ->collection()
                : localGraphNode->collection();

        auto const& shards = shardLocking.shardsForSnippet(id(), protoCol);

        return std::any_of(shards.begin(), shards.end(),
                           [&shardMapping, &server](auto const& shard) {
                             auto mappedServerIt = shardMapping.find(shard);
                             // If we find a shard here that is not in this mapping,
                             // we have 1) a problem with locking before that should have thrown
                             // 2) a problem with shardMapping lookup that should have thrown before
                             TRI_ASSERT(mappedServerIt != shardMapping.end());
                             return mappedServerIt->second == server;
                           });
      });

      if (!needInstanceHere) {
        return {TRI_ERROR_CLUSTER_NOT_LEADER};
      }

      // This is either one shard or a single satellite graph which is not used
      // as satellite graph.
      uint64_t numShards = 0;
      for (auto* aqlCollection : localGraphNode->collections()) {
        auto const& shards = shardLocking.shardsForSnippet(id(), aqlCollection);
        TRI_ASSERT(!shards.empty());
        for (auto const& shard : shards) {
          auto found = shardMapping.find(shard);
          TRI_ASSERT(found != shardMapping.end());
          // We should never have shards on other servers, except for satellite
          // graphs which are used that way, or satellite collections (in a
          // OneShard case) because local graphs (on DBServers) only ever occur
          // in either OneShard or SatelliteGraphs.
          TRI_ASSERT(found->second == server || localGraphNode->isUsedAsSatellite() ||
                     aqlCollection->isSatellite());
          // provide a correct translation from collection to shard
          // to be used in toVelocyPack methods of classes derived
          // from GraphNode
          localGraphNode->addCollectionToShard(aqlCollection->name(), shard);

          numShards++;
        }
      }

      TRI_ASSERT(numShards > 0);
      if (numShards == 0) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL_AQL, "Could not find a shard to instantiate for graph node when expected to");
      }

      auto foundEnoughShards = numShards == localGraphNode->collections().size();
      TRI_ASSERT(foundEnoughShards);
      if (!foundEnoughShards) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL_AQL);
      }
#endif
    } else {
      // exp.node is now either an enumerate collection, index, or modification.
      TRI_ASSERT(exp.node->getType() == ExecutionNode::ENUMERATE_COLLECTION ||
                 exp.node->getType() == ExecutionNode::INDEX ||
                 exp.node->getType() == ExecutionNode::INSERT ||
                 exp.node->getType() == ExecutionNode::UPDATE ||
                 exp.node->getType() == ExecutionNode::REMOVE ||
                 exp.node->getType() == ExecutionNode::REPLACE ||
                 exp.node->getType() == ExecutionNode::UPSERT ||
                 exp.node->getType() == ExecutionNode::MATERIALIZE);

      // It is of utmost importance that this is an ordered set of Shards.
      // We can only join identical indexes of shards for each collection
      // locally.
      std::set<ShardID> myExp;

      auto modNode = dynamic_cast<CollectionAccessingNode const*>(exp.node);
      // Only accessing nodes can end up here.
      TRI_ASSERT(modNode != nullptr);
      auto col = modNode->collection();
      // Should be hit earlier, a modification node here is required to have a collection
      TRI_ASSERT(col != nullptr);
      auto const& shards = shardLocking.shardsForSnippet(id(), col);
      for (auto const& s : shards) {
        auto check = shardMapping.find(s);
        // If we find a shard here that is not in this mapping,
        // we have 1) a problem with locking before that should have thrown
        // 2) a problem with shardMapping lookup that should have thrown before
        TRI_ASSERT(check != shardMapping.end());
        if (check->second == server || exp.isSatellite) {
          // add all shards on satellites.
          // and all shards where this server is the leader
          myExp.emplace(s);
        }
      }
      if (myExp.empty()) {
        return {TRI_ERROR_CLUSTER_NOT_LEADER};
      }
      // For all other Nodes we can inject a single shard at a time.
      // Always use the list of nodes we maintain to hold the first
      // of all shards.
      // We later use a cone mechanism to inject other shards of permutation
      auto collectionAccessingNode = dynamic_cast<CollectionAccessingNode*>(exp.node);
      TRI_ASSERT(collectionAccessingNode != nullptr);
      collectionAccessingNode->setUsedShard(*myExp.begin());
      if (exp.doExpand) {
        TRI_ASSERT(!collectionAccessingNode->isUsedAsSatellite());
        // All parts need to have exact same size, they need to be permutated pairwise!
        TRI_ASSERT(numberOfShardsToPermutate == 0 || myExp.size() == numberOfShardsToPermutate);
        // set the max loop index (note this will essentially be done only once)
        numberOfShardsToPermutate = myExp.size();
        if (numberOfShardsToPermutate > 1) {
          // Only in this case we really need to do an expansion
          // Otherwise we get away with only using the main stream for
          // this server
          // NOTE: This might differ between servers.
          // One server might require an expansion (many shards) while another does not (only one shard).
          localExpansions.emplace(exp.node, std::move(myExp));
        }
      } else {
        TRI_ASSERT(myExp.size() == 1);
      }
    }
  }  // for _expansions - end;
  return {localExpansions};
}

