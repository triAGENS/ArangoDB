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

#include "ExecutionEngine.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlItemBlockManager.h"
#include "Aql/Ast.h"
#include "Aql/AsyncPrefetchSlotsManager.h"
#include "Aql/BlocksWithClients.h"
#include "Aql/ClusterQuery.h"
#include "Aql/Collection.h"
#include "Aql/EngineInfoContainerCoordinator.h"
#include "Aql/EngineInfoContainerDBServerServerBased.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionNode/ExecutionNode.h"
#include "Aql/ExecutionNode/GatherNode.h"
#include "Aql/ExecutionNode/GraphNode.h"
#include "Aql/ExecutionNode/LimitNode.h"
#include "Aql/ExecutionNode/RemoteNode.h"
#include "Aql/ExecutionNode/ReturnNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Executor/IdExecutor.h"
#include "Aql/Executor/RemoteExecutor.h"
#include "Aql/Executor/ReturnExecutor.h"
#include "Aql/OptimizerRule.h"
#include "Aql/QueryContext.h"
#include "Aql/SharedQueryState.h"
#include "Aql/SkipResult.h"
#include "Assertions/ProdAssert.h"
#include "Async/async.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/RebootTracker.h"
#include "Cluster/ServerState.h"
#include "Containers/FlatHashMap.h"
#include "Logger/LogMacros.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "VocBase/Methods/Queries.h"

#include <absl/strings/str_cat.h>

namespace arangodb::aql {

namespace {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE

// Validating fullCount usage.
// For fullCount the following applies:
// 1. There can at most be one LIMIT that has fullCount: true
// 2. Limit with fullCount:true cannot be within a subquery.
// 3. Between the “root()” of the Plan (which in most cases is the RETURN
//    Node) and the LIMIT with fullCount:true no other LIMIT is allowed
//    (exception the ContrainedSort case now)
struct LimitFullCountChecker final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::Unique> {
  size_t subqueryRecursionCounter{0};
  bool seenFullCount{false};
  bool seenFullCountLimitWithoutSort{false};

  LimitFullCountChecker() = default;

  void after(ExecutionNode* en) override {
    switch (en->getType()) {
      case ExecutionNode::LIMIT: {
        auto const* limitNode = ExecutionNode::castTo<LimitNode const*>(en);
        if (limitNode->fullCount()) {
          TRI_ASSERT(!seenFullCount);                 // rule 1
          TRI_ASSERT(0 == subqueryRecursionCounter);  // rule 2
          auto const* dependency = en->getFirstDependency();
          seenFullCountLimitWithoutSort =
              !dependency || dependency->getType() != ExecutionNode::SORT;
          seenFullCount = true;
        } else {
          if (0 == subqueryRecursionCounter) {
            TRI_ASSERT(!seenFullCountLimitWithoutSort);  // rule 3
          }
        }

      } break;
      case ExecutionNode::SUBQUERY_START:
        ++subqueryRecursionCounter;
        break;
      case ExecutionNode::SUBQUERY_END:
        TRI_ASSERT(subqueryRecursionCounter > 0);
        --subqueryRecursionCounter;
        break;
      default:
        // noop
        break;
    }
  }

  /// @brief return true to enter subqueries, false otherwise
  bool enterSubquery(ExecutionNode* /*super*/,
                     ExecutionNode* /*sub*/) override {
    ++subqueryRecursionCounter;
    return true;
  }

  virtual void leaveSubquery(ExecutionNode* /*super*/,
                             ExecutionNode* /*sub*/) override {
    TRI_ASSERT(subqueryRecursionCounter > 0);
    --subqueryRecursionCounter;
  }
};
#endif
}  // namespace

/**
 * @brief Create AQL blocks from a list of ExectionNodes
 * Only works in cluster mode
 *
 * @param nodes The list of Nodes => Blocks
 * @param queryIds A Mapping: RemoteNodeId -> DBServerId -> [snippetId]
 *
 * @return A result containing the error in bad case.
 */
Result ExecutionEngine::createBlocks(std::vector<ExecutionNode*> const& nodes,
                                     MapRemoteToSnippet const& queryIds) {
  TRI_ASSERT(arangodb::ServerState::instance()->isCoordinator());

  containers::FlatHashMap<ExecutionNode*, ExecutionBlock*> cache;
  RemoteNode* remoteNode = nullptr;

  // We need to traverse the nodes from back to front, the walker collects
  // them in the wrong ordering
  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
    auto en = *it;
    auto const nodeType = en->getType();

    if (nodeType == ExecutionNode::REMOTE) {
      remoteNode = ExecutionNode::castTo<RemoteNode*>(en);
      continue;  // handled on GatherNode
    }

    // for all node types but REMOTEs, we create blocks
    auto block = en->createBlock(*this);

    if (!block) {
      return {TRI_ERROR_INTERNAL, "illegal node type"};
    }

    // transfers ownership
    // store the pointer to the block
    auto eb = addBlock(std::move(block));

    for (auto const& dep : en->getDependencies()) {
      auto d = cache.find(dep);

      if (d != cache.end()) {
        // add regular dependencies
        TRI_ASSERT((*d).second != nullptr);
        eb->addDependency((*d).second);
      }
    }

    if (nodeType == ExecutionNode::GATHER) {
      // we found a gather node
      if (en->getFirstDependency()->getType() == ExecutionNode::REMOTE) {
        TRI_ASSERT(en->getFirstDependency() == remoteNode);
        // now we'll create a remote node for each shard and add it to the
        // gather node (eb->addDependency)
        auto serversForRemote = queryIds.find(remoteNode->id());
        // Planning gone terribly wrong. The RemoteNode does not have a
        // counter-part to fetch data from.
        TRI_ASSERT(serversForRemote != queryIds.end());
        if (serversForRemote == queryIds.end()) {
          return {TRI_ERROR_INTERNAL,
                  "Did not find a DBServer to contact for RemoteNode"};
        }

        // use "server:" instead of "shard:" to send query fragments to
        // the correct servers, even after failover or when a follower drops
        // the problem with using the previous shard-based approach was that
        // responsibilities for shards may change at runtime.
        // however, an AQL query must send all requests for the query to the
        // initially used servers.
        // if there is a failover while the query is executing, we must still
        // send all following requests to the same servers, and not the newly
        // responsible servers.
        // otherwise we potentially would try to get data from a query from
        // server B while the query was only instanciated on server A.
        for (auto const& serverToSnippet : serversForRemote->second) {
          std::string const& serverID = serverToSnippet.first;
          for (std::string const& snippetId : serverToSnippet.second) {
            remoteNode->queryId(snippetId);
            remoteNode->server(serverID);
            remoteNode->setDistributeId({""});
            std::unique_ptr<ExecutionBlock> r = remoteNode->createBlock(*this);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
            auto remoteBlock =
                dynamic_cast<ExecutionBlockImpl<RemoteExecutor>*>(r.get());
            TRI_ASSERT(remoteBlock->server() == serverID);
            TRI_ASSERT(remoteBlock->distributeId() ==
                       "");  // NOLINT(readability-container-size-empty)
            TRI_ASSERT(remoteBlock->queryId() == snippetId);
#endif

            TRI_ASSERT(r != nullptr);
            eb->addDependency(r.get());
            addBlock(std::move(r));
          }
        }
      } else if (en->getFirstDependency()->getType() == ExecutionNode::ASYNC) {
        // Nothing needs to be done here in this case. Dependencies are
        // handled before.
      } else {
        return {TRI_ERROR_INTERNAL, "expecting a RemoteNode or a MutexNode"};
      }
    }

    // the last block is always the root
    root(eb);

    // put it into our cache:
    cache.try_emplace(en, eb);
  }
  return {};
}

/// @brief create the engine
ExecutionEngine::ExecutionEngine(EngineId eId, QueryContext& query,
                                 AqlItemBlockManager& itemBlockMgr,
                                 std::shared_ptr<SharedQueryState> sqs)
    : _engineId(eId),
      _query(query),
      _itemBlockManager(itemBlockMgr),
      _sharedState(std::move(sqs)),
      _blocks(),
      _root(nullptr),
      _resultRegister(RegisterId::maxRegisterId),
      _initializeCursorCalled(false),
      _asyncPrefetchSlotsManager(query.vocbase()
                                     .server()
                                     .getFeature<QueryRegistryFeature>()
                                     .asyncPrefetchSlotsManager()),
      _asyncPrefetchSlotsReservation(_asyncPrefetchSlotsManager, 0) {
  TRI_ASSERT(_sharedState != nullptr);
  _blocks.reserve(8);
}

/// @brief destroy the engine, frees all assigned blocks
ExecutionEngine::~ExecutionEngine() {
  if (_sharedState) {  // ensure no async task is working anymore
    _sharedState->invalidate();
  }
}

void ExecutionEngine::leaseAsyncPrefetchSlots(size_t value) {
  _asyncPrefetchSlotsReservation = _asyncPrefetchSlotsManager.leaseSlots(value);
}

size_t ExecutionEngine::asyncPrefetchSlotsLeased() const noexcept {
  return _asyncPrefetchSlotsReservation.value();
}

struct SingleServerQueryInstanciator final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
  ExecutionEngine& engine;
  ExecutionBlock* root{};
  containers::FlatHashMap<ExecutionNode*, ExecutionBlock*> cache;
  size_t asyncPrefetchSlotsLeft;

  explicit SingleServerQueryInstanciator(ExecutionEngine& engine) noexcept
      : engine(engine),
        asyncPrefetchSlotsLeft(engine.asyncPrefetchSlotsLeased()) {}

  void after(ExecutionNode* en) override {
    if (en->getType() == ExecutionNode::TRAVERSAL ||
        en->getType() == ExecutionNode::SHORTEST_PATH ||
        en->getType() == ExecutionNode::ENUMERATE_PATHS) {
      // We have to prepare the options before we build the block
      ExecutionNode::castTo<GraphNode*>(en)->prepareOptions();
    }

    ExecutionBlock* block = nullptr;
    if (!arangodb::ServerState::instance()->isDBServer()) {
      auto const nodeType = en->getType();

      if (nodeType == ExecutionNode::DISTRIBUTE ||
          nodeType == ExecutionNode::SCATTER ||
          (nodeType == ExecutionNode::GATHER &&
           // simon: parallel traversals use a GatherNode
           static_cast<GatherNode*>(en)->parallelism() !=
               GatherNode::Parallelism::Parallel)) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_INTERNAL, "logic error, got cluster node in local query");
      }
    } else {
      auto const& cached = cache.find(en);
      if (cached != cache.end()) {
        block = cached->second;
        TRI_ASSERT(block != nullptr);
      }
    }

    if (block == nullptr) {
      if (en->isAsyncPrefetchEnabled()) {
        if (asyncPrefetchSlotsLeft > 0) {
          --asyncPrefetchSlotsLeft;
        } else {
          en->setIsAsyncPrefetchEnabled(false);
        }
      }
      block = engine.addBlock(en->createBlock(engine));
      TRI_ASSERT(block != nullptr);
      // We have visited this node earlier, so we got its dependencies
      // Now add dependencies:
      for (auto const& it : en->getDependencies()) {
        auto it2 = cache.find(it);
        TRI_ASSERT(it2 != cache.end());
        TRI_ASSERT(it2->second != nullptr);
        block->addDependency(it2->second);
      }

      cache.try_emplace(en, block);
    }
    TRI_ASSERT(block != nullptr);

    // do we need to adjust the root node?
    if (!en->hasParent()) {
      // yes. found a new root!
      root = block;
    }
  }

  // Override this method for DBServers, there it is now possible to visit the
  // same block twice
  bool done(ExecutionNode* en) override { return false; }
};

// Here is a description of how the instantiation of an execution plan
// works in the cluster. See below for a complete example
//
// The instantiation of this works as follows:
// (0) Variable usage and register planning is done in the global plan
// (1) A walk with subqueries is done on the whole plan
//     The purpose is to plan how many ExecutionEngines we need, where they
//     have to be instantiated and which plan nodes belong to each of them.
//     Such a walk is depth first and visits subqueries after it has visited
//     the dependencies of the subquery node recursively. Whenever the
//     walk passes by a RemoteNode it switches location between coordinator
//     and DBserver and starts a new engine. The nodes of an engine are
//     collected in the after method.
//     This walk results in a list of engines and a list of nodes for
//     each engine. It follows that the order in these lists is as follows:
//     The first engine is the main one on the coordinator, it has id 0.
//     The order of the engines is exactly as they are discovered in the
//     walk. That is, engines closer to the root are earlier and engines
//     in subqueries are later. The nodes in each engine are always
//     done in a way such that a dependency D of a node N is earlier in the
//     list as N, and a subquery node is later in the list than the nodes
//     of the subquery.
// (2) buildEngines is called with that data. It proceeds engine by engine,
//     starting from the back of the list. This means that an engine that
//     is referred to in a RemoteNode (because its nodes are dependencies
//     of that node) are always already instantiated before the RemoteNode
//     is instantiated. The corresponding query ids are collected in a
//     global hash table, for which the key consists of the id of the
//     RemoteNode using the query and the actual query id. For each engine,
//     the nodes are instantiated along the list of nodes for that engine.
//     This means that all dependencies of a node N are already instantiated
//     when N is instantiated. We distinguish the coordinator and the
//     DBserver case. In the former one we have to clone a part of the
//     plan and in the latter we have to send a part to a DBserver via HTTP.
//
// Here is a fully worked out example:
//
// FOR i IN [1,2]
//   FOR d IN coll
//     FILTER d.pass == i
//     LET s = (FOR e IN coll2 FILTER e.name == d.name RETURN e)
//     RETURN {d:d, s:s}
//
// this is optimized to, variable and register planning is done in this plan:
//
//    Singleton
//        ^
//   EnumList [1,2]             Singleton
//        ^                         ^
//     Scatter (2)            Enum coll2
//        ^                         ^
//     Remote              Calc e.name==d.name
//        ^                         ^
//    Enum coll                  Filter (3)
//        ^                         ^
//  Calc d.pass==i               Remote
//        ^                         ^
//     Filter (1)                Gather
//        ^                         ^
//     Remote                    Return
//        ^                         ^
//     Gather                       |
//        ^                         |
//     Subquery  -------------------/
//        ^
//  Calc {d:d, s:s}
//        ^
//      Return (0)
//
// There are 4 engines here, their corresponding root nodes are labelled
// in the above picture in round brackets with the ids of the engine.
// Engines 1 and 3 have to be replicated for each shard of coll or coll2
// respectively, and sent to the right DBserver via HTTP. Engine 0 is the
// main one on the coordinator and engine 2 is a non-main part on the
// coordinator. Recall that the walk goes first to the dependencies before
// it visits the nodes of the subquery. Thus, the walk builds up the lists
// in this order:
//   engine 0: [Remote, Gather, Remote, Gather, Return, Subquery, Calc, Return]
//   engine 1: [Remote, Enum coll, Calc d.pass==i, Filter]
//   engine 2: [Singleton, EnumList [1,2], Scatter]
//   engine 3: [Singleton, Enum coll2, Calc e.name==d.name, Filter]
// buildEngines will then do engines in the order 3, 2, 1, 0 and for each
// of them the nodes from left to right in these lists. In the end, we have
// a proper instantiation of the whole thing.

struct DistributedQueryInstanciator final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
 private:
  EngineInfoContainerCoordinator _coordinatorParts;
  EngineInfoContainerDBServerServerBased _dbserverParts;
  bool _isCoordinator;
  bool const _pushToSingleServer;
  QueryId _lastClosed;
  Query& _query;
  // This is a handle to the last gather node that we see while traversing the
  // plan The guarantee is that we only have the combination `Remote <- Gather
  // <- before` Therefore we will always assert that this is NULLPTR with the
  // only exception of this case.
  GatherNode const* _lastGatherNode;
  std::unordered_map<ExecutionNodeId, ExecutionNode*> const& _nodesById;

 public:
  DistributedQueryInstanciator(
      Query& query,
      std::unordered_map<ExecutionNodeId, ExecutionNode*> const& nodesById,
      bool pushToSingleServer)
      : _dbserverParts(query),
        _isCoordinator(true),
        _pushToSingleServer(pushToSingleServer),
        _lastClosed(0),
        _query(query),
        _lastGatherNode(nullptr),
        _nodesById(nodesById) {}

  /// @brief before method for collection of pieces phase
  ///        Collects all nodes on the path and divides them
  ///        into coordinator and dbserver parts
  bool before(ExecutionNode* en) override final {
    auto const nodeType = en->getType();
    if (_isCoordinator) {
      _coordinatorParts.addNode(en);

      switch (nodeType) {
        case ExecutionNode::GATHER:
          _lastGatherNode = ExecutionNode::castTo<GatherNode const*>(en);
          break;
        case ExecutionNode::ASYNC:
          // We are in an GATHER/ASYNC case.
          // We are not allowed to use this case node
          // for our remote, so discard it.
          _lastGatherNode = nullptr;
          break;
        case ExecutionNode::REMOTE:
          // Flip over to DBServer
          _isCoordinator = false;
          TRI_ASSERT(_lastGatherNode != nullptr);
          _dbserverParts.openSnippet(_lastGatherNode, en->id());
          _lastGatherNode = nullptr;
          break;
        case ExecutionNode::TRAVERSAL:
        case ExecutionNode::SHORTEST_PATH:
        case ExecutionNode::ENUMERATE_PATHS:
          _dbserverParts.addGraphNode(ExecutionNode::castTo<GraphNode*>(en),
                                      _pushToSingleServer);
          break;
        default:
          // Do nothing
          break;
      }
      // lastGatherNode <=> nodeType is gather
      TRI_ASSERT((_lastGatherNode != nullptr) ==
                 (nodeType == ExecutionNode::GATHER));
    } else {
      // on dbserver
      _dbserverParts.addNode(en, _pushToSingleServer);
      // switch back from DB server to coordinator, if we are not pushing the
      // entire plan to the DB server
      if (ExecutionNode::REMOTE == nodeType) {
        TRI_ASSERT(!_pushToSingleServer);
        _isCoordinator = true;
        _coordinatorParts.openSnippet(en->id());
      }
    }

    // Always return false to not abort searching
    return false;
  }

  void after(ExecutionNode* en) override final {
    if (en->getType() == ExecutionNode::REMOTE) {
      if (_isCoordinator) {
        _lastClosed = _coordinatorParts.closeSnippet();
        _isCoordinator = false;
      } else {
        _dbserverParts.closeSnippet(_lastClosed);
        _isCoordinator = true;
      }
    }
  }

  /// @brief Builds the Engines necessary for the query execution
  ///        For Coordinator Parts:
  ///        * Creates the ExecutionBlocks
  ///        * Injects all Parts but the First one into QueryRegistery
  ///        For DBServer Parts
  ///        * Creates one Query-Entry for each Snippet per Shard (multiple on
  ///        the same DB) Each Snippet knows all details about locking.
  ///        * Only the first snippet does lock the collections.
  ///        other snippets are not responsible for any locking.
  ///        * After this step DBServer-Collections are locked!
  ///
  ///        Error Case:
  ///        * It is guaranteed that all DBServers will be send a request
  ///        to remove query snippets / locks they have locally created.
  ///        * No Engines for this query will remain in the Coordinator
  ///        Registry.
  ///        * In case the Network is broken, all non-reachable DBServers will
  ///        clean out their snippets after a TTL.
  ///        Returns the First Coordinator Engine, the one not in the registry.
  async<Result> buildEngines() {
    TRI_ASSERT(ServerState::instance()->isCoordinator());

    // QueryIds are filled by responses of DBServer parts.
    MapRemoteToSnippet snippetIds{};

    ServerQueryIdList& srvrQryId = _query.serverQueryIds();
    SnippetList& snippets = _query.snippets();

    std::map<ExecutionNodeId, ExecutionNodeId> nodeAliases;
    Result res = co_await _dbserverParts.buildEngines(_nodesById, snippetIds,
                                                      srvrQryId, nodeAliases);
    if (res.fail()) {
      co_return res;
    }

    // The coordinator engines cannot decide on lock issues later on,
    // however every engine gets injected the list of locked shards.
    res = _coordinatorParts.buildEngines(_query, _query.itemBlockManager(),
                                         snippetIds, snippets);
    if (res.fail()) {
      co_return res;
    }

    TRI_ASSERT(snippets.size() > 0);
    TRI_ASSERT(snippets[0]->engineId() == 0);

    {
      // install reboot trackers for all participating DB servers.
      // we do this so we have a quick shutdown of queries if one of the
      // participating DB servers fails. while it is not necessary for
      // correctness to fail quickly, it can be beneficial to avoid carrying out
      // a lot of operations on other servers only to realize at query end that
      // the query cannot be committed everywhere.
      auto engine = snippets[0].get();

      ClusterInfo& ci =
          _query.vocbase().server().getFeature<ClusterFeature>().clusterInfo();
      engine->rebootTrackers().reserve(srvrQryId.size());
      DatabaseFeature& df =
          _query.vocbase().server().getFeature<DatabaseFeature>();

      for (auto const& [server, queryId, rebootId] : srvrQryId) {
        TRI_ASSERT(!server.starts_with("server:"));
        std::function<void(void)> f = [srvr = server, id = _query.id(),
                                       vn = _query.vocbase().name(), &df]() {
          LOG_TOPIC("d2554", INFO, Logger::QUERIES)
              << "killing query " << id << " because participating DB server "
              << srvr << " is unavailable";
          try {
            methods::Queries::kill(df, vn, id);
          } catch (...) {
            // it does not really matter if this fails.
            // if the coordinator contacts the failed DB server next time, it
            // will realize it has failed.
          }
        };

        engine->rebootTrackers().emplace_back(ci.rebootTracker().callMeOnChange(
            {server, rebootId}, std::move(f),
            absl::StrCat("AQL query from coordinator ",
                         ServerState::instance()->getId())));
      }
    }

    bool knowsAllQueryIds = snippetIds.empty() || !srvrQryId.empty();
    TRI_ASSERT(knowsAllQueryIds);
    for (auto const& [server, queryId, rebootId] : srvrQryId) {
      if (queryId == 0) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_INTERNAL, absl::StrCat("no query ID known for ", server));
      }
    }

    TRI_ASSERT(snippets[0]->engineId() == 0);
    _query.executionStatsGuard().doUnderLock([&](auto& executionStats) {
      executionStats.setAliases(std::move(nodeAliases));
    });

    co_return res;
  }
};

std::pair<ExecutionState, Result> ExecutionEngine::initializeCursor(
    SharedAqlItemBlockPtr&& items, size_t pos) {
  if (_query.killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }
  InputAqlItemRow inputRow{CreateInvalidInputRowHint{}};
  if (items != nullptr) {
    inputRow = InputAqlItemRow{std::move(items), pos};
  }
  auto res = _root->initializeCursor(inputRow);
  if (res.first != ExecutionState::WAITING) {
    _initializeCursorCalled = true;
  }
  return res;
}

auto ExecutionEngine::execute(AqlCallStack const& stack)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  if (_query.killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  TRI_IF_FAILURE("ExecutionEngine::directKillBeforeAQLQueryExecute") {
    _query.debugKillQuery();
  }

  auto const res = _root->execute(stack);

  TRI_IF_FAILURE("ExecutionEngine::directKillAfterAQLQueryExecute") {
    _query.debugKillQuery();
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (std::get<ExecutionState>(res) == ExecutionState::WAITING) {
    auto const skipped = std::get<SkipResult>(res);
    auto const block = std::get<SharedAqlItemBlockPtr>(res);
    TRI_ASSERT(skipped.nothingSkipped());
    TRI_ASSERT(block == nullptr);
  }
#endif
  return res;
}

auto ExecutionEngine::executeForClient(AqlCallStack const& stack,
                                       std::string const& clientId)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  if (_query.killed()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_KILLED);
  }

  auto rootBlock = dynamic_cast<BlocksWithClients*>(root());
  if (rootBlock == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL_AQL,
        absl::StrCat("unexpected node type ",
                     root()->getPlanNode()->getTypeString()));
  }

  auto const res = rootBlock->executeForClient(stack, clientId);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (std::get<ExecutionState>(res) == ExecutionState::WAITING) {
    auto const skipped = std::get<SkipResult>(res);
    auto const& block = std::get<SharedAqlItemBlockPtr>(res);
    TRI_ASSERT(skipped.nothingSkipped());
    TRI_ASSERT(block == nullptr);
  }
#endif
  return res;
}

// @brief create an execution engine from a plan
async<void> ExecutionEngine::instantiateFromPlan(Query& query,
                                                 ExecutionPlan& plan,
                                                 bool planRegisters) {
  auto const role = ServerState::instance()->getRole();

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (ServerState::instance()->isCoordinator() ||
      ServerState::instance()->isSingleServer()) {
    LimitFullCountChecker limitChecker;
    plan.root()->walkSubqueriesFirst(limitChecker);
  }
#endif
  plan.findVarUsage();
  if (planRegisters) {
    plan.planRegisters();
    plan.findCollectionAccessVariables();
  }

  ExecutionBlock* root = nullptr;
  ExecutionEngine* engine = nullptr;
#ifdef USE_ENTERPRISE
  bool const pushToSingleServer = plan.hasAppliedRule(
      static_cast<int>(OptimizerRule::RuleLevel::clusterOneShardRule));
#else
  bool const pushToSingleServer = false;
#endif

  auto& mgr = query.itemBlockManager();
  initializeConstValueBlock(plan, query.bindParameters(), mgr);

  aql::SnippetList& snippets = query.snippets();
  TRI_ASSERT(snippets.empty() || ServerState::instance()->isClusterRole(role));

#ifdef USE_ENTERPRISE
  std::map<aql::ExecutionNodeId, aql::ExecutionNodeId> aliases;
  if (arangodb::ServerState::isSingleServerOrCoordinator(role)) {
    ExecutionEngine::parallelizeTraversals(query, plan, aliases);
  }
#endif

  if (ServerState::isCoordinator(role)) {
    // distributed query
    DistributedQueryInstanciator inst(query, plan.getNodesById(),
                                      pushToSingleServer);
    plan.root()->flatWalk(inst, true);

    Result res = co_await inst.buildEngines();
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    TRI_ASSERT(snippets.size() > 0);
    TRI_ASSERT(snippets[0]->engineId() == 0);
    engine = snippets[0].get();
    root = snippets[0]->root();
  } else {
    // instantiate the engine on a local server
    EngineId eId =
        arangodb::ServerState::isDBServer(role) ? TRI_NewTickServer() : 0;

    TRI_ASSERT(!ServerState::isCoordinator(role));
    bool isClusterQuery = dynamic_cast<ClusterQuery*>(&query) != nullptr;
    std::shared_ptr<SharedQueryState> sharedState;
    if (isClusterQuery) {
      ADB_PROD_ASSERT(ServerState::isDBServer(role));
      ADB_PROD_ASSERT(query.sharedState() == nullptr);
      sharedState =
          std::make_shared<SharedQueryState>(query.vocbase().server());
    } else {
      ADB_PROD_ASSERT(query.sharedState() != nullptr);
      sharedState = query.sharedState();
    }
    // we have to ensure that each ExecutionEngine has its own SharedQueryState,
    // because different snippets (engines) can operate concurrently and each
    // needs to have its own wakeupCallback
    auto retEngine = std::make_unique<ExecutionEngine>(eId, query, mgr,
                                                       std::move(sharedState));

#ifdef USE_ENTERPRISE
    query.executionStatsGuard().doUnderLock([&](auto& executionStats) {
      for (auto const& pair : aliases) {
        executionStats.addAlias(pair.first, pair.second);
      }
    });
#endif

    // lease "slots" for async prefetching operations for the current query.
    // we are tracking per server node how many slots we have already handed
    // out, so that a server does not overcommit on the number of actually
    // used async prefetching slots.
    // limiting the number of async prefetching slots helps to keep the number
    // of queued operations at bay, especially when large queries come in
    // that would use large amounts of async prefetching slots.
    // the QueryRegistryFeature keeps an overview of how many slots have been
    // leased by the currently running queries.
    retEngine->leaseAsyncPrefetchSlots(plan.asyncPrefetchNodes());

    SingleServerQueryInstanciator inst(*retEngine);
    plan.root()->walk(inst);

    root = inst.root;
    engine = retEngine.get();

    snippets.emplace_back(std::move(retEngine));
  }

  TRI_ASSERT(root != nullptr);

  engine->setupEngineRoot(*root);

  TRI_ASSERT(snippets.size() == 1 ||
             ServerState::instance()->isClusterRole(role));
}

void arangodb::aql::ExecutionEngine::setupEngineRoot(ExecutionBlock& root) {
  // inspect the root block of the query
  if (root.getPlanNode()->getType() == ExecutionNode::RETURN) {
    // it's a return node. now tell it to not copy its results from above,
    // but directly return it. we also need to note the RegisterId the
    // caller needs to look into when fetching the results

    // in short: this avoids copying the return values

    bool const returnInheritedResults =
        ExecutionNode::castTo<ReturnNode const*>(root.getPlanNode())
            ->returnInheritedResults();

    // Only spliced subqueries are supported anymore
    // ADB_PROD_ASSERT(!returnInheritedResults);
    if (returnInheritedResults) {
      auto executor = dynamic_cast<ExecutionBlockImpl<
          IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>*>(&root);
      TRI_ASSERT(executor != nullptr);
      resultRegister(executor->getOutputRegisterId());
    } else {
      auto executor = dynamic_cast<ExecutionBlockImpl<ReturnExecutor>*>(&root);
      TRI_ASSERT(executor != nullptr);
      // the ReturnExecutor always writes its output into register 0
      resultRegister(RegisterId(0));
    }
  }

  _root = &root;  // simon: otherwise it breaks
}

void arangodb::aql::ExecutionEngine::initFromPlanForCalculation(
    ExecutionPlan& plan) {
  plan.findVarUsage();
  plan.planRegisters(ExplainRegisterPlan::No);
  initializeConstValueBlock(plan, BindParameters{_query.resourceMonitor()},
                            _itemBlockManager);

  SingleServerQueryInstanciator inst(*this);
  plan.root()->walk(inst);
  TRI_ASSERT(inst.root);
  setupEngineRoot(*inst.root);
}

void ExecutionEngine::initializeConstValueBlock(
    ExecutionPlan& plan, BindParameters const& bindParameters,
    AqlItemBlockManager& mgr) {
  auto registerPlan = plan.root()->getRegisterPlan();
  auto nrConstRegs = registerPlan->nrConstRegs;
  if (nrConstRegs > 0 && mgr.getConstValueBlock() == nullptr) {
    mgr.initializeConstValueBlock(nrConstRegs);
    plan.getAst()->variables()->visit(
        [&, regPlan = plan.root()->getRegisterPlan(),
         block = mgr.getConstValueBlock()](Variable* var) {
          if (var->type() == Variable::Type::Const) {
            RegisterId reg = regPlan->variableToOptionalRegisterId(var->id);
            if (reg.value() != RegisterId::maxRegisterId) {
              TRI_ASSERT(reg.isConstRegister());
              AqlValue value = var->constantValue();
              TRI_ASSERT(!value.isNone());
              // the constValueBlock takes ownership, so we have to create a
              // copy here.
              block->emplaceValue(0, reg.value(), AqlValue(value.slice()));
            }
          } else if (var->type() == Variable::Type::BindParameter) {
            RegisterId reg = regPlan->variableToOptionalRegisterId(var->id);
            if (reg.value() != RegisterId::maxRegisterId) {
              auto [slice, node] = bindParameters.get(var->bindParameterName());
              if (slice.isNone()) {
                THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_BIND_PARAMETER_MISSING);
              }

              block->emplaceValue(0, reg.value(), AqlValue(slice));
            }
          }
        });
  }
  TRI_ASSERT(nrConstRegs == 0 ||
             mgr.getConstValueBlock()->numRegisters() == nrConstRegs);
}

/// @brief add a block to the engine
ExecutionBlock* ExecutionEngine::addBlock(
    std::unique_ptr<ExecutionBlock> block) {
  TRI_ASSERT(block != nullptr);

  // TODO track resource usage
  _blocks.emplace_back(block.get());
  return block.release();
}

void arangodb::aql::ExecutionEngine::reset() {
  _root = nullptr;
  _blocks.clear();
  _resultRegister = RegisterId{RegisterId::maxRegisterId};
  _initializeCursorCalled = false;
  _sharedState.reset();
}

ExecutionBlock* ExecutionEngine::root() const {
  TRI_ASSERT(_root != nullptr);
  return _root;
}

void ExecutionEngine::root(ExecutionBlock* root) {
  TRI_ASSERT(root != nullptr);
  _root = root;
}

QueryContext& ExecutionEngine::getQuery() const { return _query; }

bool ExecutionEngine::initializeCursorCalled() const {
  return _initializeCursorCalled;
}

void ExecutionEngine::resultRegister(RegisterId resultRegister) {
  _resultRegister = resultRegister;
}

RegisterId ExecutionEngine::resultRegister() const { return _resultRegister; }

AqlItemBlockManager& ExecutionEngine::itemBlockManager() {
  return _itemBlockManager;
}

///  @brief collected execution stats
void ExecutionEngine::collectExecutionStats(ExecutionStats& stats) {
  for (auto& block : _blocks) {
    block->collectExecStats(stats);
  }
}

std::vector<arangodb::cluster::CallbackGuard>&
ExecutionEngine::rebootTrackers() {
  return _rebootTrackers;
}

std::shared_ptr<SharedQueryState> const& ExecutionEngine::sharedState() const {
  return _sharedState;
}

}  // namespace arangodb::aql
