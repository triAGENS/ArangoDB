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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#include "GraphManager.h"
#include "GraphOperations.h"

#include <velocypack/Buffer.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <array>
#include <boost/range/join.hpp>
#include <utility>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AstNode.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryOptions.h"
#include "Basics/ReadLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Graph/Graph.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Sharding/ShardingInfo.h"
#include "Transaction/Methods.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#ifdef USE_V8
#include "Transaction/V8Context.h"
#endif
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/CollectionCreationInfo.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Properties/CreateCollectionBody.h"
#include "VocBase/Properties/DatabaseConfiguration.h"

using namespace arangodb;
using namespace arangodb::graph;
using VelocyPackHelper = basics::VelocyPackHelper;

namespace {
static bool arrayContainsCollection(VPackSlice array,
                                    std::string const& colName) {
  TRI_ASSERT(array.isArray());
  for (VPackSlice it : VPackArrayIterator(array)) {
    if (it.stringView() == colName) {
      return true;
    }
  }
  return false;
}
}  // namespace

std::shared_ptr<transaction::Context> GraphManager::ctx() const {
#ifdef USE_V8
  // we must use v8
  return transaction::V8Context::createWhenRequired(_vocbase, _operationOrigin,
                                                    true);
#else
  return transaction::StandaloneContext::create(_vocbase, _operationOrigin);
#endif
}

bool GraphManager::renameGraphCollection(std::string const& oldName,
                                         std::string const& newName) {
  // todo: return a result, by now just used in the graph modules

  std::vector<std::unique_ptr<Graph>> renamedGraphs;

  auto callback = [&](std::unique_ptr<Graph> graph) -> Result {
    bool renamed = graph->renameCollections(oldName, newName);
    if (renamed) {
      renamedGraphs.emplace_back(std::move(graph));
    }
    return Result{};
  };
  Result res = applyOnAllGraphs(callback);
  if (res.fail()) {
    return false;
  }

  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::WRITE);
  res = trx.begin();

  if (!res.ok()) {
    return false;
  }
  OperationOptions options(ExecContext::current());

  for (auto const& graph : renamedGraphs) {
    VPackBuilder builder;
    builder.openObject();
    graph->toPersistence(builder);
    builder.close();

    try {
      OperationResult opRes =
          trx.update(StaticStrings::GraphCollection, builder.slice(), options);
      if (opRes.fail()) {
        res = trx.finish(opRes.result);
        if (res.fail()) {
          return false;
        }
      }
    } catch (...) {
    }
  }

  res = trx.finish(Result());  // if we get here, it was a success
  if (res.fail()) {
    return false;
  }
  return true;
}

Result GraphManager::checkForEdgeDefinitionConflicts(
    std::map<std::string, EdgeDefinition> const& edgeDefinitions,
    std::string const& graphName) const {
  auto callback = [&](std::unique_ptr<Graph> graph) -> Result {
    if (graph->name() == graphName) {
      // No need to check our graph
      return {TRI_ERROR_NO_ERROR};
    }

    for (auto const& sGED : graph->edgeDefinitions()) {
      std::string const& col = sGED.first;
      auto it = edgeDefinitions.find(col);
      if (it != edgeDefinitions.end()) {
        if (sGED.second != it->second) {
          // found an incompatible edge definition for the same collection
          return Result(
              TRI_ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS,
              sGED.first + " " +
                  std::string{TRI_errno_string(
                      TRI_ERROR_GRAPH_COLLECTION_USE_IN_MULTI_GRAPHS)});
        }
      }
    }
    return {TRI_ERROR_NO_ERROR};
  };
  return applyOnAllGraphs(callback);
}

Result GraphManager::findOrCreateCollectionsByEdgeDefinition(
    Graph& graph, EdgeDefinition const& edgeDefinition, bool waitForSync) {
  std::unordered_set<std::string> satellites = graph.satelliteCollections();
  // Validation Phase collect a list of collections to create
  std::unordered_set<std::string> documentCollectionsToCreate{};
  std::unordered_set<std::string> edgeCollectionsToCreate{};
  std::unordered_set<std::shared_ptr<LogicalCollection>>
      existentDocumentCollections{};
  std::unordered_set<std::shared_ptr<LogicalCollection>>
      existentEdgeCollections{};

  auto& vocbase = ctx()->vocbase();
  std::string const& edgeCollName = edgeDefinition.getName();
  std::shared_ptr<LogicalCollection> edgeColl;
  Result res = methods::Collections::lookup(vocbase, edgeCollName, edgeColl);
  if (res.ok()) {
    TRI_ASSERT(edgeColl);
    if (edgeColl->type() != TRI_COL_TYPE_EDGE) {
      return Result(
          TRI_ERROR_GRAPH_EDGE_DEFINITION_IS_DOCUMENT,
          "Collection: '" + edgeColl->name() + "' is not an EdgeCollection");
    } else {
      // found the collection
      existentEdgeCollections.emplace(edgeColl);
    }
  } else if (!res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
    return res;
  } else {
    edgeCollectionsToCreate.emplace(edgeCollName);
  }

  for (auto const& vertexColl : edgeDefinition.getFrom()) {
    std::shared_ptr<LogicalCollection> col;
    Result res = methods::Collections::lookup(vocbase, vertexColl, col);
    if (res.ok()) {
      TRI_ASSERT(col);
      if (col->isSatellite()) {
        satellites.emplace(col->name());
      }
      existentDocumentCollections.emplace(col);
    } else if (!res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
      return res;
    } else {
      if (edgeCollectionsToCreate.find(vertexColl) ==
          edgeCollectionsToCreate.end()) {
        auto res = ensureVertexShardingMatches(graph, *edgeColl, satellites,
                                               vertexColl, true);
        if (res.fail()) {
          return res;
        }
        documentCollectionsToCreate.emplace(vertexColl);
      }
    }
  }

  for (auto const& vertexColl : edgeDefinition.getTo()) {
    std::shared_ptr<LogicalCollection> col;
    Result res = methods::Collections::lookup(vocbase, vertexColl, col);
    if (res.ok()) {
      TRI_ASSERT(col);
      if (col->isSatellite()) {
        satellites.emplace(col->name());
      }
      existentDocumentCollections.emplace(col);
    } else if (!res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
      return res;
    } else {
      if (edgeCollectionsToCreate.find(vertexColl) ==
          edgeCollectionsToCreate.end()) {
        if (edgeColl) {
          auto res = ensureVertexShardingMatches(graph, *edgeColl, satellites,
                                                 vertexColl, false);
          if (res.fail()) {
            return res;
          }
        }

        documentCollectionsToCreate.emplace(vertexColl);
      }
    }
  }
  return ensureCollections(graph, documentCollectionsToCreate,
                           edgeCollectionsToCreate, existentDocumentCollections,
                           existentEdgeCollections, satellites, waitForSync);
}

/// @brief extract the collection by either id or name, may return nullptr!
std::shared_ptr<LogicalCollection> GraphManager::getCollectionByName(
    const TRI_vocbase_t& vocbase, std::string const& name) {
  if (!name.empty()) {
    // try looking up the collection by name then
    if (arangodb::ServerState::instance()->isRunningInCluster()) {
      ClusterInfo& ci =
          vocbase.server().getFeature<ClusterFeature>().clusterInfo();
      return ci.getCollectionNT(vocbase.name(), name);
    } else {
      return vocbase.lookupCollection(name);
    }
  }

  return nullptr;
}

bool GraphManager::graphExists(std::string const& graphName) const {
  VPackBuilder checkDocument;
  {
    VPackObjectBuilder guard(&checkDocument);
    checkDocument.add(StaticStrings::KeyString, VPackValue(graphName));
  }

  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::READ);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = trx.begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  OperationOptions options;
  OperationResult checkDoc = trx.document(StaticStrings::GraphCollection,
                                          checkDocument.slice(), options);
  return trx.finish(checkDoc.result).ok();
}

ResultT<std::unique_ptr<Graph>> GraphManager::lookupGraphByName(
    std::string const& name) const {
  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::READ);

  Result res = trx.begin();

  if (res.fail()) {
    std::stringstream ss;
    ss << "while looking up graph '" << name << "': " << res.errorMessage();
    res.reset(res.errorNumber(), ss.str());
    return {res};
  }

  VPackBuilder b;
  {
    VPackObjectBuilder guard(&b);
    b.add(StaticStrings::KeyString, VPackValue(name));
  }

  // Default options are enough here
  OperationOptions options;

  OperationResult result =
      trx.document(StaticStrings::GraphCollection, b.slice(), options);

  // Commit or abort.
  res = trx.finish(result.result);

  if (result.fail()) {
    if (result.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
      std::string msg = basics::Exception::FillExceptionString(
          TRI_ERROR_GRAPH_NOT_FOUND, name.c_str());
      return Result{TRI_ERROR_GRAPH_NOT_FOUND, std::move(msg)};
    } else {
      return Result{result.errorNumber(),
                    "while looking up graph '" + name + "'"};
    }
  }

  if (res.fail()) {
    std::stringstream ss;
    ss << "while looking up graph '" << name << "': " << res.errorMessage();
    res.reset(res.errorNumber(), ss.str());
    return {res};
  }
  return {Graph::fromPersistence(_vocbase, result.slice())};
}

OperationResult GraphManager::createGraph(VPackSlice document,
                                          bool waitForSync) const {
  OperationOptions options(ExecContext::current());
  VPackSlice graphNameSlice = document.get("name");
  if (!graphNameSlice.isString()) {
    return OperationResult{TRI_ERROR_GRAPH_CREATE_MISSING_NAME, options};
  }
  std::string const graphName = graphNameSlice.copyString();

  if (graphExists(graphName)) {
    return OperationResult{TRI_ERROR_GRAPH_DUPLICATE, options};
  }

  auto graphRes = buildGraphFromInput(graphName, document);
  if (graphRes.fail()) {
    return OperationResult{std::move(graphRes).result(), options};
  }
  // Guaranteed to not be nullptr
  std::unique_ptr<Graph> graph = std::move(graphRes.get());
  TRI_ASSERT(graph != nullptr);

  // check permissions
  Result res = checkCreateGraphPermissions(graph.get());
  if (res.fail()) {
    return OperationResult{res, options};
  }

  // check edgeDefinitionConflicts
  res =
      checkForEdgeDefinitionConflicts(graph->edgeDefinitions(), graph->name());
  if (res.fail()) {
    return OperationResult{res, options};
  }

  // Make sure all collections exist and are created
  res = ensureAllCollections(graph.get(), waitForSync);
  if (res.fail()) {
    return OperationResult{res, options};
  }

  // finally save the graph
  return storeGraph(*(graph.get()), waitForSync, false);
}

OperationResult GraphManager::storeGraph(Graph const& graph, bool waitForSync,
                                         bool isUpdate) const {
  VPackBuilder builder;
  builder.openObject();
  graph.toPersistence(builder);
  builder.close();

  // Here we need a second transaction.
  // If now someone has created a graph with the same name
  // in the meanwhile, sorry bad luck.
  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::WRITE);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  OperationOptions options(ExecContext::current());
  options.waitForSync = waitForSync;
  Result res = trx.begin();
  if (res.fail()) {
    return OperationResult{std::move(res), options};
  }
  OperationResult result = isUpdate ? trx.update(StaticStrings::GraphCollection,
                                                 builder.slice(), options)
                                    : trx.insert(StaticStrings::GraphCollection,
                                                 builder.slice(), options);

  res = trx.finish(result.result);
  if (res.fail() && result.ok()) {
    return OperationResult{std::move(res), options};
  }
  return result;
}

Result GraphManager::applyOnAllGraphs(
    std::function<Result(std::unique_ptr<Graph>)> const& callback) const {
  std::string const queryStr{"FOR g IN _graphs RETURN g"};
  aql::QueryOptions options;
  options.skipAudit = true;

  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      _vocbase, _operationOrigin, aql::QueryString(queryStr), nullptr,
      std::move(options));
  auto queryResult = std::move(queryFuture.waitAndGet());

  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
      return {TRI_ERROR_REQUEST_CANCELED};
    }
    return queryResult.result;
  }

  VPackSlice graphsSlice = queryResult.data->slice();
  if (graphsSlice.isNone()) {
    return {TRI_ERROR_OUT_OF_MEMORY};
  } else if (!graphsSlice.isArray()) {
    LOG_TOPIC("cbe2c", ERR, arangodb::Logger::GRAPHS)
        << "cannot read graphs from _graphs collection";
    return {TRI_ERROR_GRAPH_INTERNAL_DATA_CORRUPT,
            "Cannot read graphs from _graphs collection"};
  }
  Result res;
  for (VPackSlice it : VPackArrayIterator(graphsSlice)) {
    std::unique_ptr<Graph> graph;
    try {
      graph = Graph::fromPersistence(_vocbase, it.resolveExternals());
    } catch (basics::Exception const& e) {
      return {e.code(), e.message()};
    }
    TRI_ASSERT(graph != nullptr);
    if (graph == nullptr) {
      return {TRI_ERROR_OUT_OF_MEMORY};
    }
    res = callback(std::move(graph));
    if (res.fail()) {
      return res;
    }
  }
  TRI_ASSERT(res.ok());
  return res;
}

Result GraphManager::ensureAllCollections(Graph* graph,
                                          bool waitForSync) const {
  TRI_ASSERT(graph != nullptr);
  std::unordered_set<std::string> satellites = graph->satelliteCollections();
  // Validation Phase collect a list of collections to create
  std::unordered_set<std::string> documentCollectionsToCreate{};
  std::unordered_set<std::string> edgeCollectionsToCreate{};
  std::unordered_set<std::shared_ptr<LogicalCollection>>
      existentDocumentCollections{};
  std::unordered_set<std::shared_ptr<LogicalCollection>>
      existentEdgeCollections{};

  auto& vocbase = ctx()->vocbase();
  Result innerRes{TRI_ERROR_NO_ERROR};

  // I. Check which collections do exists and which not.
  // Check that all edgeCollections are either to be created
  // or exist in a valid way.
  // a) edge collections
  for (auto const& edgeColl : graph->edgeCollections()) {
    std::shared_ptr<LogicalCollection> col;
    Result res = methods::Collections::lookup(vocbase, edgeColl, col);
    if (res.ok()) {
      TRI_ASSERT(col);
      if (col->type() != TRI_COL_TYPE_EDGE) {
        return Result(
            TRI_ERROR_GRAPH_EDGE_DEFINITION_IS_DOCUMENT,
            "Collection: '" + col->name() + "' is not an EdgeCollection");
      } else {
        // found the collection
        existentEdgeCollections.emplace(std::move(col));
      }
    } else if (!res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
      return res;
    } else {
      edgeCollectionsToCreate.emplace(edgeColl);
    }
  }

  // Check that all vertexCollections are either to be created
  // or exist in a valid way.
  // b) vertex collections
  for (auto const& vertexColl : graph->vertexCollections()) {
    std::shared_ptr<LogicalCollection> col;
    Result res = methods::Collections::lookup(vocbase, vertexColl, col);
    if (res.ok()) {
      TRI_ASSERT(col);
      if (col->isSatellite()) {
        satellites.emplace(col->name());
      }
      existentDocumentCollections.emplace(col);
    } else if (!res.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
      return res;
    } else {
      if (edgeCollectionsToCreate.find(vertexColl) ==
          edgeCollectionsToCreate.end()) {
        documentCollectionsToCreate.emplace(vertexColl);
      }
    }
  }
  return ensureCollections(*graph, documentCollectionsToCreate,
                           edgeCollectionsToCreate, existentDocumentCollections,
                           existentEdgeCollections, satellites, waitForSync);
}

Result GraphManager::ensureCollections(
    Graph& graph, std::unordered_set<std::string>& documentCollectionsToCreate,
    std::unordered_set<std::string> const& edgeCollectionsToCreate,
    std::unordered_set<std::shared_ptr<LogicalCollection>> const&
        existentDocumentCollections,
    std::unordered_set<std::shared_ptr<LogicalCollection>> const&
        existentEdgeCollections,
    std::unordered_set<std::string> const& satellites, bool waitForSync) const {
  CollectionNameResolver resolver(_vocbase);
  auto getLeaderName = [&](LogicalCollection const& col) -> std::string {
    auto const& distLike = col.distributeShardsLike();
    if (distLike.empty()) {
      return col.name();
    }
    if (ServerState::instance()->isRunningInCluster()) {
      return resolver.getCollectionNameCluster(
          DataSourceId{basics::StringUtils::uint64(distLike)});
    }
    return distLike;
  };

  auto anyExistingCollection =
      std::invoke([&]() -> std::shared_ptr<LogicalCollection> {
        if (!existentDocumentCollections.empty()) {
          // Prefer Vertex collections
          for (auto const& col : existentDocumentCollections) {
            // We need to ignore satelliteCollections on SmartGraphs
            if (!graph.isSmart() || !col->isSatellite()) {
              return col;
            }
          }
        }
        if (!existentEdgeCollections.empty()) {
          // over edge collections
          for (auto const& col : existentEdgeCollections) {
            // We need to ignore satelliteCollections on SmartGraphs
            if (!graph.isSmart() || !col->isSatellite()) {
              return col;
            }
          }
        }
        return nullptr;
      });

  auto config = _vocbase.getDatabaseConfiguration();
  std::optional<std::string> leadingCollection = std::nullopt;
  bool pickedExisting = false;

  if (config.isOneShardDB) {
    leadingCollection = config.defaultDistributeShardsLike;
    TRI_ASSERT(leadingCollection.has_value() &&
               !leadingCollection.value().empty());
    pickedExisting = true;
  } else {
    // This will only have effect in Enterprise Version
    std::tie(leadingCollection, pickedExisting) = graph.getLeadingCollection(
        documentCollectionsToCreate, edgeCollectionsToCreate, satellites,
        anyExistingCollection, getLeaderName);
  }

  // Validate if the existing collections can be used within this graph type.

  // document collections
  for (auto const& col : existentDocumentCollections) {
    Result res =
        graph.validateCollection(*col, leadingCollection, getLeaderName);
    if (res.fail()) {
      return res;
    }
  }
  // edge collections
  for (auto const& col : existentEdgeCollections) {
    Result res =
        graph.validateCollection(*col, leadingCollection, getLeaderName);
    if (res.fail()) {
      return res;
    }
  }

  std::vector<CreateCollectionBody> createRequests;

  if (leadingCollection.has_value() && graph.requiresInitialUpdate()) {
    // We create the leader within this call, rewire distributeShardsLike
    // lookup:
    auto originalCallback = config.getCollectionGroupSharding;
    config.getCollectionGroupSharding =
        [&leadingCollection, &createRequests,
         originalCallback = std::move(originalCallback)](
            std::string const& name) -> ResultT<UserInputCollectionProperties> {
      // We can only search for the leading collection.
      TRI_ASSERT(name == leadingCollection.value())
          << name << " does not match " << leadingCollection.value();
      for (auto const& c : createRequests) {
        if (c.name == name) {
          // On new graphs the leading collection is in the first position.
          // So we will quickly loop here, even if it is not this loop is safe
          return c;
        }
      }
      // Try lookup via Database
      return originalCallback(name);
    };
  }
  for (auto const& c : documentCollectionsToCreate) {
    auto col = graph.prepareCreateCollectionBodyVertex(c, leadingCollection,
                                                       satellites, config);
    if (col.fail()) {
      return col.result();
    }
    createRequests.emplace_back(std::move(col.get()));
  }

  for (auto const& c : edgeCollectionsToCreate) {
    auto col = graph.prepareCreateCollectionBodyEdge(c, leadingCollection,
                                                     satellites, config);
    if (col.fail()) {
      return col.result();
    }
    createRequests.emplace_back(std::move(col.get()));
  }
  if (createRequests.empty()) {
    // Nothing to do.
    if (leadingCollection.has_value() && graph.requiresInitialUpdate()) {
      // We can only end up here if we have an existing collection
      TRI_ASSERT(anyExistingCollection != nullptr);
      TRI_ASSERT(pickedExisting);
      graph.updateInitial({anyExistingCollection}, leadingCollection,
                          getLeaderName);
    }
    return {};
  }

#ifdef USE_ENTERPRISE
  bool const allowEnterpriseCollectionsOnSingleServer =
      ServerState::instance()->isSingleServer() &&
      (graph.isSmart() || graph.isSatellite());
#else
  bool const allowEnterpriseCollectionsOnSingleServer = false;
#endif
  auto& cluster = _vocbase.server().getFeature<ClusterFeature>();
  bool waitForSyncReplication = cluster.createWaitsForSyncReplication();

  OperationOptions opOptions(ExecContext::current());
  auto finalResult = methods::Collections::create(
      ctx()->vocbase(), opOptions, std::move(createRequests),
      waitForSyncReplication, true, false,
      allowEnterpriseCollectionsOnSingleServer);
  // We do not care for the Collections here, just forward the result
  // API guarantees all or none.
  if (finalResult.ok() && leadingCollection.has_value() &&
      graph.requiresInitialUpdate()) {
    if (pickedExisting) {
      if (config.isOneShardDB) {
        // We need to shard by the default sharding collection
        // Take initial from the selected existing one
        auto defaultSharding =
            resolver.getCollection(config.defaultDistributeShardsLike);
        ADB_PROD_ASSERT(defaultSharding != nullptr)
            << "We have lost the leading collection of a oneShardDatabase";
        graph.updateInitial({std::move(defaultSharding)}, leadingCollection,
                            getLeaderName);
      } else {
        // Take initial from the selected existing one
        graph.updateInitial({anyExistingCollection}, leadingCollection,
                            getLeaderName);
      }
    } else {
      // Take the initial from freshly created collections
      graph.updateInitial(finalResult.get(), leadingCollection, getLeaderName);
    }
  }
  return finalResult.result();
}

bool GraphManager::onlySatellitesUsed(Graph const* graph) const {
  for (auto const& cname : graph->vertexCollections()) {
    if (!_vocbase.lookupCollection(cname).get()->isSatellite()) {
      return false;
    }
  }

  for (auto const& cname : graph->edgeCollections()) {
    if (!_vocbase.lookupCollection(cname).get()->isSatellite()) {
      return false;
    }
  }

  return true;
}

Result GraphManager::readGraphs(velocypack::Builder& builder) const {
  std::string const queryStr{
      "FOR g IN _graphs RETURN MERGE(g, {name: g._key})"};
  return readGraphByQuery(builder, queryStr);
}

Result GraphManager::readGraphKeys(velocypack::Builder& builder) const {
  std::string const queryStr{"FOR g IN _graphs RETURN g._key"};
  return readGraphByQuery(builder, queryStr);
}

Result GraphManager::readGraphByQuery(velocypack::Builder& builder,
                                      std::string const& queryStr) const {
  auto query = arangodb::aql::Query::create(
      ctx(), arangodb::aql::QueryString(queryStr), nullptr);
  query->queryOptions().skipAudit = true;

  LOG_TOPIC("f6782", DEBUG, arangodb::Logger::GRAPHS)
      << "starting to load graphs information";
  auto aborter = std::make_shared<aql::QueryAborter>(query);
  aql::QueryResult queryResult = query->executeSync(aborter);

  if (queryResult.result.fail()) {
    if (queryResult.result.is(TRI_ERROR_REQUEST_CANCELED) ||
        (queryResult.result.is(TRI_ERROR_QUERY_KILLED))) {
      return Result(TRI_ERROR_REQUEST_CANCELED);
    }
    return std::move(queryResult.result);
  }

  VPackSlice graphsSlice = queryResult.data->slice();

  if (graphsSlice.isNone()) {
    return Result(TRI_ERROR_OUT_OF_MEMORY);
  }
  if (!graphsSlice.isArray()) {
    LOG_TOPIC("338b7", ERR, arangodb::Logger::GRAPHS)
        << "cannot read graphs from _graphs collection";
  }

  builder.add(VPackValue(VPackValueType::Object));
  builder.add("graphs", graphsSlice);
  builder.close();

  return Result(TRI_ERROR_NO_ERROR);
}

Result GraphManager::checkForEdgeDefinitionConflicts(
    arangodb::graph::EdgeDefinition const& edgeDefinition,
    std::string const& graphName) const {
  std::map<std::string, EdgeDefinition> edgeDefs{
      std::make_pair(edgeDefinition.getName(), edgeDefinition)};

  return checkForEdgeDefinitionConflicts(edgeDefs, graphName);
}

Result GraphManager::checkCreateGraphPermissions(Graph const* graph) const {
  std::string const& databaseName = ctx()->vocbase().name();

  std::stringstream stringstream;
  stringstream << "When creating graph " << databaseName << "." << graph->name()
               << ": ";
  std::string const logprefix = stringstream.str();

  ExecContext const& execContext = ExecContext::current();
  if (!ExecContext::isAuthEnabled()) {
    LOG_TOPIC("952c0", DEBUG, Logger::GRAPHS)
        << logprefix << "Permissions are turned off.";
    return TRI_ERROR_NO_ERROR;
  }

  // Test if we are allowed to modify _graphs first.
  // Note that this check includes the following check in the loop
  //   if (!collectionExists(col) && !canUseDatabaseRW)
  // as canUseDatabase(RW) <=> canUseCollection("_...", RW).
  // However, in case a collection has to be created but can't, we have to
  // throw FORBIDDEN instead of READ_ONLY for backwards compatibility.
  if (!execContext.canUseDatabase(auth::Level::RW)) {
    // Check for all collections: if it exists and if we have RO access to it.
    // If none fails the check above we need to return READ_ONLY.
    // Otherwise we return FORBIDDEN
    auto checkCollectionAccess = [&](std::string const& col) -> bool {
      // We need RO on all collections. And, in case any collection does not
      // exist, we need RW on the database.
      if (!collectionExists(col)) {
        LOG_TOPIC("ca4de", DEBUG, Logger::GRAPHS)
            << logprefix << "Cannot create collection " << databaseName << "."
            << col;
        return false;
      }
      if (!execContext.canUseCollection(col, auth::Level::RO)) {
        LOG_TOPIC("b4d48", DEBUG, Logger::GRAPHS)
            << logprefix << "No read access to " << databaseName << "." << col;
        return false;
      }
      return true;
    };

    // Test all edge Collections
    for (auto const& it : graph->edgeCollections()) {
      if (!checkCollectionAccess(it)) {
        return {TRI_ERROR_FORBIDDEN,
                "Createing Graphs requires RW access on the database (" +
                    databaseName + ")"};
      }
    }

    // Test all vertex Collections
    for (auto const& it : graph->vertexCollections()) {
      if (!checkCollectionAccess(it)) {
        return {TRI_ERROR_FORBIDDEN,
                "Createing Graphs requires RW access on the database (" +
                    databaseName + ")"};
      }
    }

    LOG_TOPIC("89b89", DEBUG, Logger::GRAPHS)
        << logprefix << "No write access to " << databaseName << "."
        << StaticStrings::GraphCollection;
    return {TRI_ERROR_ARANGO_READ_ONLY,
            "Createing Graphs requires RW access on the database (" +
                databaseName + ")"};
  }

  auto checkCollectionAccess = [&](std::string const& col) -> bool {
    // We need RO on all collections. And, in case any collection does not
    // exist, we need RW on the database.
    if (!execContext.canUseCollection(col, auth::Level::RO)) {
      LOG_TOPIC("43c84", DEBUG, Logger::GRAPHS)
          << logprefix << "No read access to " << databaseName << "." << col;
      return false;
    }
    return true;
  };

  // Test all edge Collections
  for (auto const& it : graph->edgeCollections()) {
    if (!checkCollectionAccess(it)) {
      return TRI_ERROR_FORBIDDEN;
    }
  }

  // Test all vertex Collections
  for (auto const& it : graph->vertexCollections()) {
    if (!checkCollectionAccess(it)) {
      return TRI_ERROR_FORBIDDEN;
    }
  }
  return TRI_ERROR_NO_ERROR;
}

bool GraphManager::collectionExists(std::string const& collection) const {
  return getCollectionByName(ctx()->vocbase(), collection) != nullptr;
}

OperationResult GraphManager::removeGraph(Graph const& graph, bool waitForSync,
                                          bool dropCollections) {
  // the set of collections that have no distributeShardsLike attribute
  std::unordered_set<std::string> leadersToBeRemoved;
  // the set of collections that have a distributeShardsLike attribute, they are
  // removed before the collections from leadersToBeRemoved
  std::unordered_set<std::string> followersToBeRemoved;
  OperationOptions options(ExecContext::current());

  if (dropCollections) {
    // Puts the collection with name colName to leadersToBeRemoved (if
    // distributeShardsLike is not defined) or to followersToBeRemoved (if it
    // is defined) or does nothing if there is no collection with this name.
    auto addToRemoveCollections = [this, &graph, &leadersToBeRemoved,
                                   &followersToBeRemoved](
                                      std::string const& colName) {
      std::shared_ptr<LogicalCollection> col =
          getCollectionByName(ctx()->vocbase(), colName);
      if (col == nullptr) {
        return;
      }

      if (col->distributeShardsLike().empty()) {
        pushCollectionIfMayBeDropped(colName, graph.name(), leadersToBeRemoved);
      } else {
        pushCollectionIfMayBeDropped(colName, graph.name(),
                                     followersToBeRemoved);
      }
    };

    for (auto const& vertexCollection : graph.vertexCollections()) {
      addToRemoveCollections(vertexCollection);
    }
    for (auto const& orphanCollection : graph.orphanCollections()) {
      addToRemoveCollections(orphanCollection);
    }
    for (auto const& edgeCollection : graph.edgeCollections()) {
      addToRemoveCollections(edgeCollection);
    }
  }

  Result permRes = checkDropGraphPermissions(graph, followersToBeRemoved,
                                             leadersToBeRemoved);
  if (permRes.fail()) {
    return OperationResult{std::move(permRes), options};
  }

  VPackBuilder builder;
  {
    VPackObjectBuilder guard(&builder);
    builder.add(StaticStrings::KeyString, VPackValue(graph.name()));
  }

  {  // Remove from _graphs
    OperationOptions options(ExecContext::current());
    options.waitForSync = waitForSync;

    SingleCollectionTransaction trx{ctx(), StaticStrings::GraphCollection,
                                    AccessMode::Type::WRITE};

    Result res = trx.begin();
    if (res.fail()) {
      return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND, options);
    }
    VPackSlice search = builder.slice();
    OperationResult result =
        trx.remove(StaticStrings::GraphCollection, search, options);

    res = trx.finish(result.result);
    if (result.fail()) {
      return result;
    }
    if (res.fail()) {
      return OperationResult{res, options};
    }
    TRI_ASSERT(res.ok() && result.ok());
  }

  {  // drop collections

    Result firstDropError;
    // remove graph related collections.
    // we are not able to do this in a transaction, so doing it afterwards.
    // there may be no collections to drop when dropCollections is false.
    TRI_ASSERT(dropCollections ||
               (leadersToBeRemoved.empty() && followersToBeRemoved.empty()));
    // drop followers (with distributeShardsLike) first and leaders (which
    // occur in some distributeShardsLike) second.
    for (auto const& cname :
         boost::join(followersToBeRemoved, leadersToBeRemoved)) {
      Result dropResult;
      std::shared_ptr<LogicalCollection> coll;
      Result found =
          methods::Collections::lookup(ctx()->vocbase(), cname, coll);
      if (found.ok()) {
        TRI_ASSERT(coll);
        CollectionDropOptions dropOptions{.allowDropSystem = false,
                                          .allowDropGraphCollection = true};
        dropResult = arangodb::methods::Collections::drop(*coll, dropOptions);
      }

      if (dropResult.fail()) {
        LOG_TOPIC("04c88", WARN, Logger::GRAPHS)
            << "While removing graph `" << graph.name() << "`: "
            << "Dropping collection `" << cname << "` failed with error "
            << dropResult.errorNumber() << ": " << dropResult.errorMessage();

        // save the first error:
        if (firstDropError.ok()) {
          firstDropError = dropResult;
        }
      }
    }

    if (firstDropError.fail()) {
      return OperationResult{firstDropError, options};
    }
  }

  return OperationResult{TRI_ERROR_NO_ERROR, options};
}

Result GraphManager::pushCollectionIfMayBeDropped(
    std::string const& colName, std::string const& graphName,
    std::unordered_set<std::string>& toBeRemoved) {
  VPackBuilder graphsBuilder;
  Result result = readGraphs(graphsBuilder);
  if (result.fail()) {
    return result;
  }

  VPackSlice graphs = graphsBuilder.slice();

  bool collectionUnused = true;
  TRI_ASSERT(graphs.get("graphs").isArray());

  if (!graphs.get("graphs").isArray()) {
    return Result(TRI_ERROR_GRAPH_INTERNAL_DATA_CORRUPT);
  }

  for (auto graph : VPackArrayIterator(graphs.get("graphs"))) {
    graph = graph.resolveExternals();
    if (!collectionUnused) {
      // Short circuit
      break;
    }
    if (graph.get(StaticStrings::KeyString).stringView() == graphName) {
      continue;
    }

    // check edge definitions
    VPackSlice edgeDefinitions = graph.get(StaticStrings::GraphEdgeDefinitions);
    if (edgeDefinitions.isArray()) {
      for (auto const& edgeDefinition : VPackArrayIterator(edgeDefinitions)) {
        // edge collection
        if (edgeDefinition.get("collection").stringView() == colName) {
          collectionUnused = false;
          break;
        }
        // from's
        if (::arrayContainsCollection(
                edgeDefinition.get(StaticStrings::GraphFrom), colName)) {
          collectionUnused = false;
          break;
        }
        // to's
        if (::arrayContainsCollection(
                edgeDefinition.get(StaticStrings::GraphTo), colName)) {
          collectionUnused = false;
          break;
        }
      }
    } else {
      return Result(TRI_ERROR_GRAPH_INTERNAL_DATA_CORRUPT);
    }

    // check orphan collections
    VPackSlice orphanCollections = graph.get(StaticStrings::GraphOrphans);
    if (orphanCollections.isArray()) {
      if (::arrayContainsCollection(orphanCollections, colName)) {
        collectionUnused = false;
        break;
      }
    }
  }

  if (collectionUnused) {
    toBeRemoved.emplace(colName);
  }

  return Result(TRI_ERROR_NO_ERROR);
}

Result GraphManager::checkDropGraphPermissions(
    const Graph& graph,
    const std::unordered_set<std::string>& followersToBeRemoved,
    const std::unordered_set<std::string>& leadersToBeRemoved) {
  std::string const& databaseName = ctx()->vocbase().name();

  std::stringstream stringstream;
  stringstream << "When dropping graph " << databaseName << "." << graph.name()
               << ": ";
  std::string const logprefix = stringstream.str();

  ExecContext const& execContext = ExecContext::current();
  if (!ExecContext::isAuthEnabled()) {
    LOG_TOPIC("56c2f", DEBUG, Logger::GRAPHS)
        << logprefix << "Permissions are turned off.";
    return TRI_ERROR_NO_ERROR;
  }

  bool mustDropAtLeastOneCollection =
      !followersToBeRemoved.empty() || !leadersToBeRemoved.empty();
  bool canUseDatabaseRW = execContext.canUseDatabase(auth::Level::RW);

  if (mustDropAtLeastOneCollection && !canUseDatabaseRW) {
    LOG_TOPIC("fdc57", DEBUG, Logger::GRAPHS)
        << logprefix << "Must drop at least one collection in " << databaseName
        << ", but don't have permissions.";
    return TRI_ERROR_FORBIDDEN;
  }

  for (auto const& col :
       boost::join(followersToBeRemoved, leadersToBeRemoved)) {
    // We need RW to drop a collection.
    if (!execContext.canUseCollection(col, auth::Level::RW)) {
      LOG_TOPIC("96384", DEBUG, Logger::GRAPHS)
          << logprefix << "No write access to " << databaseName << "." << col;
      return TRI_ERROR_FORBIDDEN;
    }
  }

  // We need RW on _graphs (which is the same as RW on the database). But in
  // case we don't even have RO access, throw FORBIDDEN instead of READ_ONLY.
  if (!execContext.canUseCollection(StaticStrings::GraphCollection,
                                    auth::Level::RO)) {
    LOG_TOPIC("bfe63", DEBUG, Logger::GRAPHS)
        << logprefix << "No read access to " << databaseName << "."
        << StaticStrings::GraphCollection;
    return TRI_ERROR_FORBIDDEN;
  }

  // Note that this check includes the following check from before
  //   if (mustDropAtLeastOneCollection && !canUseDatabaseRW)
  // as canUseDatabase(RW) <=> canUseCollection("_...", RW).
  // However, in case a collection has to be created but can't, we have to
  // throw FORBIDDEN instead of READ_ONLY for backwards compatibility.
  if (!execContext.canUseCollection(StaticStrings::GraphCollection,
                                    auth::Level::RW)) {
    LOG_TOPIC("bbb09", DEBUG, Logger::GRAPHS)
        << logprefix << "No write access to " << databaseName << "."
        << StaticStrings::GraphCollection;
    return TRI_ERROR_ARANGO_READ_ONLY;
  }

  return TRI_ERROR_NO_ERROR;
}

ResultT<std::unique_ptr<Graph>> GraphManager::buildGraphFromInput(
    std::string const& graphName, VPackSlice input) const {
  auto isSatellite = [](VPackSlice options) -> bool {
    if (options.isObject()) {
      VPackSlice s = options.get(StaticStrings::ReplicationFactor);
      return ((s.isNumber() && s.getNumber<int>() == 0) ||
              (s.isString() && s.stringView() == "satellite"));
    }
    return false;
  };
  auto numberOfShards = [](VPackSlice options) -> std::pair<bool, int> {
    if (options.isObject()) {
      VPackSlice s = options.get(StaticStrings::NumberOfShards);
      if (s.isNumber()) {
        return {true, s.getNumber<int>()};
      }
    }
    return {false, 0};
  };

  try {
    TRI_ASSERT(input.isObject());

    if (ServerState::instance()->isCoordinator() ||
        ServerState::instance()->isSingleServer()) {
      VPackSlice s = input.get(StaticStrings::IsSmart);
      VPackSlice options = input.get(StaticStrings::GraphOptions);

      bool smartSet = s.isTrue();
      bool sgaSet = false;
      if (options.isObject()) {
        sgaSet =
            options.hasKey(StaticStrings::GraphSmartGraphAttribute) &&
            options.get(StaticStrings::GraphSmartGraphAttribute).isString();
      }

      if (smartSet || sgaSet) {
        std::string errParameter;
        if (smartSet) {
          errParameter = StaticStrings::IsSmart;
        } else {
          errParameter = StaticStrings::GraphSmartGraphAttribute;
        }

        if (options.isObject()) {
          if (options.hasKey(StaticStrings::GraphSatellites) &&
              !options.get(StaticStrings::GraphSatellites).isArray()) {
            return Result{TRI_ERROR_BAD_PARAMETER,
                          "Missing array for field 'satellites'"};
          }
          VPackSlice replicationFactor =
              options.get(StaticStrings::ReplicationFactor);
          if ((replicationFactor.isNumber() &&
               replicationFactor.getNumber<int>() == 0)) {
            return Result{TRI_ERROR_BAD_PARAMETER,
                          "invalid combination of '" + errParameter +
                              "' and 'replicationFactor'"};
          } else if (replicationFactor.isString() &&
                     replicationFactor.stringView() == "satellite") {
            return Result{TRI_ERROR_BAD_PARAMETER, "invalid combination of '" +
                                                       errParameter +
                                                       "' and 'satellite' "};
          }
        }
      }

      if (options.isObject() && isSatellite(options)) {
        auto ns = numberOfShards(options);
        if (ns.first && ns.second != 1) {
          // the combination of numberOfShards != 1 and replicationFactor ==
          // 'satellite' is invalid
          return Result{TRI_ERROR_BAD_PARAMETER,
                        "invalid combination of 'numberOfShards' and "
                        "'satellite' replicationFactor"};
        }
      }

      // validate numberOfShards and replicationFactor
      Result res = ShardingInfo::validateShardsAndReplicationFactor(
          options, _vocbase.server(), true);
      if (res.fail()) {
        return res;
      }
    }
    return Graph::fromUserInput(_vocbase, graphName, input,
                                input.get(StaticStrings::GraphOptions));
  } catch (arangodb::basics::Exception const& e) {
    return Result{e.code(), e.message()};
  } catch (...) {
    return {TRI_ERROR_INTERNAL};
  }
}

#ifndef USE_ENTERPRISE
Result GraphManager::ensureVertexShardingMatches(
    Graph const&, LogicalCollection&, std::unordered_set<std::string> const&,
    std::string const&, bool) const {
  // Only relevant for Enterprise graphs.
  return TRI_ERROR_NO_ERROR;
}
#endif
