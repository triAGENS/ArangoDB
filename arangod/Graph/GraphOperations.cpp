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
/// @author Tobias Gödderz & Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#include "GraphOperations.h"

#include <velocypack/Buffer.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <array>
#include <boost/range/join.hpp>
#include <utility>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Query.h"
#include "Aql/QueryAborter.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Graph/Graph.h"
#include "Graph/GraphManager.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Transaction/Methods.h"
#include "Transaction/SmartContext.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"

using namespace arangodb;
using namespace arangodb::graph;

std::shared_ptr<transaction::Context> GraphOperations::ctx() {
  if (!_ctx) {
    _ctx = std::make_shared<transaction::StandaloneContext>(_vocbase,
                                                            _operationOrigin);
  }
  return _ctx;
}

void GraphOperations::checkForUsedEdgeCollections(
    const Graph& graph, std::string const& collectionName,
    std::unordered_set<std::string>& possibleEdgeCollections) {
  for (auto const& it : graph.edgeDefinitions()) {
    if (it.second.isVertexCollectionUsed(collectionName)) {
      possibleEdgeCollections.emplace(it.second.getName());
    }
  }
}

futures::Future<OperationResult> GraphOperations::changeEdgeDefinitionForGraph(
    Graph& graph, EdgeDefinition const& newEdgeDef, bool waitForSync,
    transaction::Methods& trx) {
  OperationOptions options(ExecContext::current());
  options.waitForSync = waitForSync;

  VPackBuilder builder;
  // remove old definition, insert the new one instead
  Result res = graph.replaceEdgeDefinition(newEdgeDef);
  if (res.fail()) {
    return OperationResult(res, options);
  }

  builder.openObject();
  graph.toPersistence(builder);
  builder.close();

  GraphManager gmngr{_vocbase, _operationOrigin};
  res = gmngr.ensureAllCollections(&_graph, waitForSync);
  if (res.fail()) {
    return OperationResult(res, options);
  }

  // now write to database
  return trx.update(StaticStrings::GraphCollection, builder.slice(), options);
}

futures::Future<OperationResult> GraphOperations::eraseEdgeDefinition(
    bool waitForSync, std::string const& edgeDefinitionName,
    bool dropCollection) {
  OperationOptions options(ExecContext::current());
  options.waitForSync = waitForSync;

  // check if edgeCollection is available
  Result res = checkEdgeCollectionAvailability(edgeDefinitionName);
  if (res.fail()) {
    co_return OperationResult(res, options);
  }

  if (dropCollection && !hasRWPermissionsFor(edgeDefinitionName)) {
    co_return OperationResult{TRI_ERROR_FORBIDDEN, options};
  }

  // remove edgeDefinition from graph config
  _graph.removeEdgeDefinition(edgeDefinitionName);

  VPackBuilder builder;
  builder.openObject();
  _graph.toPersistence(builder);
  builder.close();

  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::WRITE);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  res = co_await trx.beginAsync();

  if (!res.ok()) {
    res = co_await trx.finishAsync(res);
    co_return OperationResult(res, options);
  }
  OperationResult result =
      trx.update(StaticStrings::GraphCollection, builder.slice(), options);

  if (dropCollection) {
    std::unordered_set<std::string> collectionsToBeRemoved;
    GraphManager gmngr{_vocbase, _operationOrigin};

    // add the edge collection itself for removal
    gmngr.pushCollectionIfMayBeDropped(edgeDefinitionName, _graph.name(),
                                       collectionsToBeRemoved);
    for (auto const& cname : collectionsToBeRemoved) {
      std::shared_ptr<LogicalCollection> coll;
      res = methods::Collections::lookup(_vocbase, cname, coll);
      if (res.ok()) {
        TRI_ASSERT(coll);
#ifdef USE_ENTERPRISE
        {
          if (coll->type() == TRI_COL_TYPE_DOCUMENT) {
            bool initial = isUsedAsInitialCollection(cname);
            if (initial) {
              co_return OperationResult(TRI_ERROR_GRAPH_COLLECTION_IS_INITIAL,
                                        options);
            }
          }
        }
#endif
        CollectionDropOptions dropOptions{.allowDropGraphCollection = true};
        res = methods::Collections::drop(*coll, dropOptions);
        if (res.fail()) {
          res = co_await trx.finishAsync(result.result);
          co_return OperationResult(res, options);
        }
      } else {
        res = co_await trx.finishAsync(result.result);
        co_return OperationResult(res, options);
      }
    }
  }

  res = co_await trx.finishAsync(result.result);

  if (result.ok() && res.fail()) {
    co_return OperationResult(res, options);
  }

  co_return result;
}

Result GraphOperations::checkEdgeCollectionAvailability(
    std::string const& edgeCollectionName) {
  bool found = _graph.edgeCollections().find(edgeCollectionName) !=
               _graph.edgeCollections().end();

  if (!found) {
    return Result(TRI_ERROR_GRAPH_EDGE_COLLECTION_NOT_USED);
  }

  return Result(TRI_ERROR_NO_ERROR);
}

Result GraphOperations::checkVertexCollectionAvailability(
    std::string const& vertexCollectionName,
    VertexValidationOrigin edgeDocumentOrigin) {
  // first check whether the collection is part of the graph
  bool found = false;
  if (_graph.vertexCollections().contains(vertexCollectionName) ||
      _graph.orphanCollections().contains(vertexCollectionName)) {
    found = true;
  }

  if (!found) {
    if (edgeDocumentOrigin == VertexValidationOrigin::FROM_ATTRIBUTE ||
        edgeDocumentOrigin == VertexValidationOrigin::TO_ATTRIBUTE) {
      std::string_view originString = "_from";
      if (edgeDocumentOrigin == VertexValidationOrigin::TO_ATTRIBUTE) {
        originString = "_to";
      }

      return Result(
          TRI_ERROR_GRAPH_REFERENCED_VERTEX_COLLECTION_NOT_PART_OF_THE_GRAPH,
          absl::StrCat("referenced ", originString, " collection '",
                       vertexCollectionName, "' is not part of the graph"));
    }
    return Result(TRI_ERROR_GRAPH_COLLECTION_NOT_PART_OF_THE_GRAPH);
  }

  // check if the collection is available
  std::shared_ptr<LogicalCollection> def =
      GraphManager::getCollectionByName(_vocbase, vertexCollectionName);

  if (def == nullptr) {
    return Result(TRI_ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST,
                  vertexCollectionName + " " +
                      std::string{TRI_errno_string(
                          TRI_ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST)});
  }

  return Result(TRI_ERROR_NO_ERROR);
}

futures::Future<OperationResult> GraphOperations::editEdgeDefinition(
    VPackSlice edgeDefinitionSlice, VPackSlice definitionOptions,
    bool waitForSync, std::string const& edgeDefinitionName) {
  TRI_ASSERT(definitionOptions.isObject());
  OperationOptions options(ExecContext::current());
  auto maybeEdgeDef = EdgeDefinition::createFromVelocypack(edgeDefinitionSlice);
  if (!maybeEdgeDef) {
    co_return OperationResult{std::move(maybeEdgeDef).result(), options};
  }
  EdgeDefinition const& edgeDefinition = maybeEdgeDef.get();

  // check if edgeCollection is available
  Result res = checkEdgeCollectionAvailability(edgeDefinitionName);
  if (res.fail()) {
    co_return OperationResult(res, options);
  }

  Result permRes = checkEdgeDefinitionPermissions(edgeDefinition);
  if (permRes.fail()) {
    co_return OperationResult{
        permRes,
        options,
    };
  }

  auto satData = definitionOptions.get(StaticStrings::GraphSatellites);
  if (satData.isArray()) {
    auto res = _graph.addSatellites(satData);
    if (res.fail()) {
      // Handles invalid Slice Content
      co_return OperationResult{std::move(res), options};
    }
  }

  GraphManager gmngr{_vocbase, _operationOrigin};
  res = gmngr.findOrCreateCollectionsByEdgeDefinition(_graph, edgeDefinition,
                                                      waitForSync);
  if (res.fail()) {
    co_return OperationResult(res, options);
  }

  if (!_graph.hasEdgeCollection(edgeDefinition.getName())) {
    co_return OperationResult(TRI_ERROR_GRAPH_EDGE_COLLECTION_NOT_USED,
                              options);
  }

  // change definition for ALL graphs
  VPackBuilder graphsBuilder;
  gmngr.readGraphs(graphsBuilder);
  VPackSlice graphs = graphsBuilder.slice();

  if (!graphs.get("graphs").isArray()) {
    co_return OperationResult{TRI_ERROR_GRAPH_INTERNAL_DATA_CORRUPT, options};
  }

  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::WRITE);

  res = co_await trx.beginAsync();

  if (!res.ok()) {
    co_return OperationResult(res, options);
  }

  for (auto singleGraph : VPackArrayIterator(graphs.get("graphs"))) {
    std::unique_ptr<Graph> graph =
        Graph::fromPersistence(_vocbase, singleGraph.resolveExternals());
    if (graph->hasEdgeCollection(edgeDefinition.getName())) {
      // only try to modify the edgeDefinition if it's available.
      OperationResult result = co_await changeEdgeDefinitionForGraph(
          *(graph.get()), edgeDefinition, waitForSync, trx);
      if (result.fail()) {
        co_return result;
      }
    }
  }

  res = co_await trx.finishAsync(TRI_ERROR_NO_ERROR);
  co_return OperationResult(res, options);
}

futures::Future<OperationResult> GraphOperations::addOrphanCollection(
    VPackSlice document, bool waitForSync, bool createCollection) {
  GraphManager gmngr{_vocbase, _operationOrigin};
  std::string collectionName = document.get("collection").copyString();

  std::shared_ptr<LogicalCollection> def;

  OperationOptions options(ExecContext::current());
  options.waitForSync = waitForSync;
  OperationResult result(Result(), options);

  VPackSlice editOptions = document.get(StaticStrings::GraphOptions);
  if (editOptions.isObject()) {
    editOptions = editOptions.get(StaticStrings::GraphSatellites);
    if (editOptions.isArray()) {
      auto res = _graph.addSatellites(editOptions);
      if (res.fail()) {
        co_return OperationResult(std::move(res), options);
      }
    }
  }

  if (_graph.hasVertexCollection(collectionName)) {
    if (_graph.hasOrphanCollection(collectionName)) {
      co_return OperationResult(TRI_ERROR_GRAPH_COLLECTION_USED_IN_ORPHANS,
                                options);
    }
    co_return OperationResult(
        Result(TRI_ERROR_GRAPH_COLLECTION_USED_IN_EDGE_DEF,
               collectionName + " " +
                   std::string{TRI_errno_string(
                       TRI_ERROR_GRAPH_COLLECTION_USED_IN_EDGE_DEF)}),
        options);
  }

  // add orphan collection to graph
  _graph.addOrphanCollection(std::string(collectionName));

  def = GraphManager::getCollectionByName(_vocbase, collectionName);
  Result res;

  if (def == nullptr) {
    if (createCollection) {
      // ensure that all collections are available
      res = gmngr.ensureAllCollections(&_graph, waitForSync);

      if (res.fail()) {
        co_return OperationResult{std::move(res), options};
      }
    } else {
      co_return OperationResult(
          Result(TRI_ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST,
                 collectionName + " " +
                     std::string{TRI_errno_string(
                         TRI_ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST)}),
          options);
    }
  } else {
    // Hint: Now needed because of the initial property
    res = gmngr.ensureAllCollections(&_graph, waitForSync);
    if (res.fail()) {
      co_return OperationResult{std::move(res), options};
    }

    if (def->type() != TRI_COL_TYPE_DOCUMENT) {
      co_return OperationResult(TRI_ERROR_GRAPH_WRONG_COLLECTION_TYPE_VERTEX,
                                options);
    }
    // TODO: Check if this is now actually duplicate.
    // The ensureAllCollections above fouls handle the validation, or should not
    // be called if invalid, as it has side-effects.
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
      return col.distributeShardsLike();
    };

    auto [leading, unused] =
        _graph.getLeadingCollection({}, {}, {}, nullptr, getLeaderName);
    res = _graph.validateCollection(*(def.get()), leading, getLeaderName);
    if (res.fail()) {
      co_return OperationResult{std::move(res), options};
    }
  }

  VPackBuilder builder;
  builder.openObject();
  _graph.toPersistence(builder);
  builder.close();

  SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                  AccessMode::Type::WRITE);

  res = co_await trx.beginAsync();

  if (!res.ok()) {
    co_return OperationResult(res, options);
  }

  result = co_await trx.updateAsync(StaticStrings::GraphCollection,
                                    builder.slice(), options);

  res = co_await trx.finishAsync(result.result);
  if (result.ok() && res.fail()) {
    co_return OperationResult(res, options);
  }
  co_return result;
}

futures::Future<OperationResult> GraphOperations::eraseOrphanCollection(
    bool waitForSync, std::string const& collectionName, bool dropCollection) {
  OperationOptions options(ExecContext::current());
#ifdef USE_ENTERPRISE
  {
    if (dropCollection) {
      bool initial = isUsedAsInitialCollection(collectionName);
      if (initial) {
        co_return OperationResult(TRI_ERROR_GRAPH_COLLECTION_IS_INITIAL,
                                  options);
      }
    }
  }
#endif

  // check if collection exists within the orphan collections
  bool found = false;
  for (auto const& oName : _graph.orphanCollections()) {
    if (oName == collectionName) {
      found = true;
      break;
    }
  }
  if (!found) {
    co_return OperationResult(TRI_ERROR_GRAPH_NOT_IN_ORPHAN_COLLECTION,
                              options);
  }

  // check if collection exists in the database
  bool collectionExists = true;
  Result res = checkVertexCollectionAvailability(collectionName);
  if (res.fail()) {
    if (res.is(TRI_ERROR_GRAPH_VERTEX_COL_DOES_NOT_EXIST)) {
      // in this case, we are allowed just to drop it out of the definition
      collectionExists = false;
    } else {
      co_return OperationResult(res, options);
    }
  }

  if (collectionExists && !hasRWPermissionsFor(collectionName)) {
    co_return OperationResult{TRI_ERROR_FORBIDDEN, options};
  }

  res = _graph.removeOrphanCollection(collectionName);
  if (res.fail()) {
    co_return OperationResult(res, options);
  }

  VPackBuilder builder;
  builder.openObject();
  _graph.toPersistence(builder);
  builder.close();

  OperationResult result(Result(), options);
  {
    SingleCollectionTransaction trx(ctx(), StaticStrings::GraphCollection,
                                    AccessMode::Type::WRITE);
    trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

    res = co_await trx.beginAsync();

    if (!res.ok()) {
      co_return OperationResult(res, options);
    }
    OperationOptions options;
    options.waitForSync = waitForSync;

    result =
        trx.update(StaticStrings::GraphCollection, builder.slice(), options);
    res = co_await trx.finishAsync(result.result);
  }

  if (dropCollection && collectionExists) {
    std::unordered_set<std::string> collectionsToBeRemoved;
    GraphManager gmngr{_vocbase, _operationOrigin};
    res = gmngr.pushCollectionIfMayBeDropped(collectionName, "",
                                             collectionsToBeRemoved);

    if (res.fail()) {
      co_return OperationResult(res, options);
    }

    for (auto const& cname : collectionsToBeRemoved) {
      std::shared_ptr<LogicalCollection> coll;
      res = methods::Collections::lookup(_vocbase, cname, coll);
      if (res.ok()) {
        TRI_ASSERT(coll);
        CollectionDropOptions dropOptions{.allowDropGraphCollection = true};
        res = methods::Collections::drop(*coll, dropOptions);
      }
      if (res.fail()) {
        co_return OperationResult(res, options);
      }
    }
  }

  if (result.ok() && res.fail()) {
    co_return OperationResult(res, options);
  }

  co_return result;
}

futures::Future<OperationResult> GraphOperations::addEdgeDefinition(
    VPackSlice edgeDefinitionSlice, VPackSlice definitionOptions,
    bool waitForSync) {
  TRI_ASSERT(definitionOptions.isObject());
  OperationOptions options(ExecContext::current());
  ResultT<EdgeDefinition const*> defRes =
      _graph.addEdgeDefinition(edgeDefinitionSlice);
  if (defRes.fail()) {
    co_return OperationResult(std::move(defRes).result(), options);
  }
  // Guaranteed to be non nullptr
  TRI_ASSERT(defRes.get() != nullptr);

  // ... in different graph
  GraphManager gmngr{_vocbase, _operationOrigin};

  Result res =
      gmngr.checkForEdgeDefinitionConflicts(*(defRes.get()), _graph.name());
  if (res.fail()) {
    // If this fails we will not persist.
    co_return OperationResult(res, options);
  }

  auto satData = definitionOptions.get(StaticStrings::GraphSatellites);
  if (satData.isArray()) {
    auto res = _graph.addSatellites(satData);
    if (res.fail()) {
      // Handles invalid Slice Content
      co_return OperationResult{std::move(res), options};
    }
  }

  res = gmngr.ensureAllCollections(&_graph, waitForSync);

  if (res.fail()) {
    co_return OperationResult{std::move(res), options};
  }

  // finally save the graph
  co_return gmngr.storeGraph(_graph, waitForSync, true);
}

futures::Future<OperationResult> GraphOperations::getVertex(
    std::string const& collectionName, std::string const& key,
    std::optional<RevisionId> rev) {
  // check if the vertex collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkVertexRes = checkVertexCollectionAvailability(collectionName);
  if (checkVertexRes.fail()) {
    co_return OperationResult(checkVertexRes, options);
  }
  co_return co_await getDocument(collectionName, key, std::move(rev));
}

futures::Future<OperationResult> GraphOperations::getEdge(
    std::string const& definitionName, std::string const& key,
    std::optional<RevisionId> rev) {
  // check if the edge collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkEdgeRes = checkEdgeCollectionAvailability(definitionName);
  if (checkEdgeRes.fail()) {
    co_return OperationResult(checkEdgeRes, options);
  }
  co_return co_await getDocument(definitionName, key, std::move(rev));
}

futures::Future<OperationResult> GraphOperations::getDocument(
    std::string const& collectionName, std::string const& key,
    std::optional<RevisionId> rev) {
  OperationOptions options;
  options.ignoreRevs = !rev.has_value();

  VPackBufferPtr searchBuffer = _getSearchSlice(key, rev);
  VPackSlice search{searchBuffer->data()};

  // find and load collection given by name or identifier
  SingleCollectionTransaction trx(ctx(), collectionName,
                                  AccessMode::Type::READ);
  trx.addHint(transaction::Hints::Hint::SINGLE_OPERATION);

  Result res = co_await trx.beginAsync();

  if (!res.ok()) {
    co_return OperationResult(res, options);
  }

  OperationResult result = trx.document(collectionName, search, options);

  res = co_await trx.finishAsync(result.result);

  if (result.ok() && res.fail()) {
    co_return OperationResult(res, options);
  }
  co_return result;
}

GraphOperations::VPackBufferPtr GraphOperations::_getSearchSlice(
    std::string const& key, std::optional<RevisionId>& rev) const {
  VPackBuilder builder;
  {
    VPackObjectBuilder guard(&builder);
    builder.add(StaticStrings::KeyString, VPackValue(key));
    if (rev) {
      builder.add(StaticStrings::RevString, VPackValue(rev.value().toString()));
    }
  }

  return builder.steal();
}

futures::Future<OperationResult> GraphOperations::removeEdge(
    std::string const& definitionName, std::string const& key,
    std::optional<RevisionId> rev, bool waitForSync, bool returnOld) {
  // check if the edge collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkEdgeRes = checkEdgeCollectionAvailability(definitionName);
  if (checkEdgeRes.fail()) {
    co_return OperationResult(checkEdgeRes, options);
  }

  co_return co_await removeEdgeOrVertex(definitionName, key, rev, waitForSync,
                                        returnOld);
}

futures::Future<OperationResult> GraphOperations::modifyDocument(
    std::string const& collectionName, std::string const& key,
    VPackSlice document, bool isPatch, std::optional<RevisionId> rev,
    bool waitForSync, bool returnOld, bool returnNew, bool keepNull,
    transaction::Methods& trx) {
  // extract the revision, if single document variant and header given:
  std::unique_ptr<VPackBuilder> builder;

  VPackSlice keyInBody = document.get(StaticStrings::KeyString);
  if ((rev && RevisionId::fromSlice(document) != rev.value()) ||
      keyInBody.isNone() || keyInBody.isNull() ||
      (keyInBody.isString() && keyInBody.copyString() != key)) {
    // We need to rewrite the document with the given revision and key:
    builder = std::make_unique<VPackBuilder>();
    {
      VPackObjectBuilder guard(builder.get());
      TRI_SanitizeObject(document, *builder);
      builder->add(StaticStrings::KeyString, VPackValue(key));
      if (rev) {
        builder->add(StaticStrings::RevString,
                     VPackValue(rev.value().toString()));
      }
    }
    document = builder->slice();
  }

  OperationOptions options;
  options.ignoreRevs = !rev.has_value();
  options.waitForSync = waitForSync;
  options.returnNew = returnNew;
  options.returnOld = returnOld;
  OperationResult result(Result(), options);

  if (isPatch) {
    options.keepNull = keepNull;
    result = co_await trx.updateAsync(collectionName, document, options);
  } else {
    result = co_await trx.replaceAsync(collectionName, document, options);
  }

  Result res = co_await trx.finishAsync(result.result);

  if (result.ok() && res.fail()) {
    co_return OperationResult(res, options);
  }
  co_return result;
}

futures::Future<OperationResult> GraphOperations::createDocument(
    transaction::Methods* trx, std::string const& collectionName,
    VPackSlice document, bool waitForSync, bool returnNew) {
  OperationOptions options;
  options.waitForSync = waitForSync;
  options.returnNew = returnNew;

  OperationResult result =
      co_await trx->insertAsync(collectionName, document, options);
  result.result = co_await trx->finishAsync(result.result);

  co_return result;
}

futures::Future<OperationResult> GraphOperations::updateEdge(
    std::string const& definitionName, std::string const& key,
    VPackSlice document, std::optional<RevisionId> rev, bool waitForSync,
    bool returnOld, bool returnNew, bool keepNull) {
  // check if the edge collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkEdgeRes = checkEdgeCollectionAvailability(definitionName);
  if (checkEdgeRes.fail()) {
    co_return OperationResult(checkEdgeRes, options);
  }

  auto [res, trx] =
      co_await validateEdge(definitionName, document, waitForSync, true);
  if (res.fail()) {
    // cppcheck-suppress returnStdMoveLocal
    co_return std::move(res);
  }
  TRI_ASSERT(trx != nullptr);

  co_return co_await modifyDocument(definitionName, key, document, true,
                                    std::move(rev), waitForSync, returnOld,
                                    returnNew, keepNull, *trx.get());
}

futures::Future<OperationResult> GraphOperations::replaceEdge(
    std::string const& definitionName, std::string const& key,
    VPackSlice document, std::optional<RevisionId> rev, bool waitForSync,
    bool returnOld, bool returnNew, bool keepNull) {
  // check if the edge collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkEdgeRes = checkEdgeCollectionAvailability(definitionName);
  if (checkEdgeRes.fail()) {
    co_return OperationResult(checkEdgeRes, options);
  }

  auto [res, trx] =
      co_await validateEdge(definitionName, document, waitForSync, false);
  if (res.fail()) {
    // cppcheck-suppress returnStdMoveLocal
    co_return std::move(res);
  }
  TRI_ASSERT(trx != nullptr);

  co_return co_await modifyDocument(definitionName, key, document, false,
                                    std::move(rev), waitForSync, returnOld,
                                    returnNew, keepNull, *trx.get());
}

futures::Future<
    std::pair<OperationResult, std::unique_ptr<transaction::Methods>>>
GraphOperations::validateEdge(std::string const& definitionName,
                              const VPackSlice& document, bool waitForSync,
                              bool isUpdate) {
  std::string fromCollectionName;
  std::string fromCollectionKey;
  std::string toCollectionName;
  std::string toCollectionKey;

  auto [res, foundEdgeDefinition] =
      validateEdgeContent(document, fromCollectionName, fromCollectionKey,
                          toCollectionName, toCollectionKey, isUpdate);
  if (res.fail()) {
    co_return std::make_pair(std::move(res), nullptr);
  }

  std::vector<std::string> readCollections;
  std::vector<std::string> writeCollections;
  std::vector<std::string> exclusiveCollections;

  if (foundEdgeDefinition) {
    readCollections.emplace_back(fromCollectionName);
    readCollections.emplace_back(toCollectionName);
  }
  writeCollections.emplace_back(definitionName);

  transaction::Options trxOptions;
  trxOptions.waitForSync = waitForSync;

  auto trx = std::make_unique<transaction::Methods>(
      ctx(), readCollections, writeCollections, exclusiveCollections,
      trxOptions);

  Result tRes = co_await trx->beginAsync();

  OperationOptions options(ExecContext::current());

  if (!tRes.ok()) {
    co_return std::make_pair(OperationResult(tRes, options), nullptr);
  }

  if (foundEdgeDefinition) {
    res = validateEdgeVertices(fromCollectionName, fromCollectionKey,
                               toCollectionName, toCollectionKey, *trx.get());

    if (res.fail()) {
      co_return std::make_pair(std::move(res), nullptr);
    }
  }

  co_return std::make_pair(std::move(res), std::move(trx));
}

OperationResult GraphOperations::validateEdgeVertices(
    std::string const& fromCollectionName, std::string const& fromCollectionKey,
    std::string const& toCollectionName, std::string const& toCollectionKey,
    transaction::Methods& trx) {
  VPackBuilder bT;
  {
    VPackObjectBuilder guard(&bT);
    bT.add(StaticStrings::KeyString, VPackValue(toCollectionKey));
  }

  VPackBuilder bF;
  {
    VPackObjectBuilder guard(&bF);
    bF.add(StaticStrings::KeyString, VPackValue(fromCollectionKey));
  }

  OperationOptions options(ExecContext::current());
  OperationResult resultFrom =
      trx.document(fromCollectionName, bF.slice(), options);
  OperationResult resultTo =
      trx.document(toCollectionName, bT.slice(), options);

  // actual result doesn't matter here
  if (resultFrom.ok() && resultTo.ok()) {
    return OperationResult(TRI_ERROR_NO_ERROR, options);
  } else {
    return OperationResult(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND, options);
  }
}

std::pair<OperationResult, bool> GraphOperations::validateEdgeContent(
    const VPackSlice& document, std::string& fromCollectionName,
    std::string& fromCollectionKey, std::string& toCollectionName,
    std::string& toCollectionKey, bool isUpdate) {
  VPackSlice fromStringSlice = document.get(StaticStrings::FromString);
  VPackSlice toStringSlice = document.get(StaticStrings::ToString);
  OperationOptions options(ExecContext::current());

  // Validate from & to slices or
  // validate from || to slices
  auto validateSlices =
      [&](std::optional<VPackSlice> fromSlice,
          std::optional<VPackSlice> toSlice) -> OperationResult {
    if (fromSlice.has_value()) {
      std::string fromString = fromSlice.value().copyString();

      // _from part
      size_t pos = fromString.find('/');
      if (pos != std::string::npos) {
        fromCollectionName = fromString.substr(0, pos);
        // check if vertex collections are part of the graph definition
        auto foundFromRes = checkVertexCollectionAvailability(
            fromCollectionName, VertexValidationOrigin::FROM_ATTRIBUTE);
        if (foundFromRes.fail()) {
          return OperationResult(foundFromRes, options);
        }

        fromCollectionKey = fromString.substr(pos + 1, fromString.length());
      } else {
        return OperationResult(TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE,
                               options);
      }
    }

    if (toSlice.has_value()) {
      std::string toString = toSlice.value().copyString();
      // _to part
      size_t pos = toString.find('/');
      if (pos != std::string::npos) {
        toCollectionName = toString.substr(0, pos);
        auto foundToRes = checkVertexCollectionAvailability(
            toCollectionName, VertexValidationOrigin::TO_ATTRIBUTE);
        if (foundToRes.fail()) {
          return OperationResult(foundToRes, options);
        }
        toCollectionKey = toString.substr(pos + 1, toString.length());
      } else {
        return OperationResult(TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE,
                               options);
      }
    }

    return OperationResult(TRI_ERROR_NO_ERROR, options);
  };

  if (!fromStringSlice.isString() || !toStringSlice.isString()) {
    if (isUpdate) {
      // if the document is already available, we still need to do a
      // partial validation. This is because the document might have
      // only _from or only _to attributes defined. Which is totally fine
      // and valid.

      if (fromStringSlice.isString()) {
        // validate _from attribute
        auto sliceRes = validateSlices(fromStringSlice, std::nullopt);
        if (sliceRes.fail()) {
          return std::make_pair(std::move(sliceRes), false);
        }
      }
      if (toStringSlice.isString()) {
        // validate _to attribute
        auto sliceRes = validateSlices(std::nullopt, toStringSlice);
        if (sliceRes.fail()) {
          return std::make_pair(std::move(sliceRes), false);
        }
      }
      return std::make_pair(OperationResult(TRI_ERROR_NO_ERROR, options),
                            false);
    }
    return std::make_pair(
        OperationResult(TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE, options),
        false);
  }

  auto sliceRes = validateSlices(fromStringSlice, toStringSlice);
  if (sliceRes.fail()) {
    return std::make_pair(std::move(sliceRes), true);
  }

  return std::make_pair(OperationResult(TRI_ERROR_NO_ERROR, options), true);
}

futures::Future<OperationResult> GraphOperations::createEdge(
    std::string const& definitionName, VPackSlice document, bool waitForSync,
    bool returnNew) {
  // check if edgeCollection is available in the graph definition
  OperationOptions options(ExecContext::current());
  Result checkEdgeRes = checkEdgeCollectionAvailability(definitionName);
  if (checkEdgeRes.fail()) {
    co_return OperationResult(checkEdgeRes, options);
  }

  auto [res, trx] =
      co_await validateEdge(definitionName, document, waitForSync, false);
  if (res.fail()) {
    // cppcheck-suppress returnStdMoveLocal
    co_return std::move(res);
  }
  TRI_ASSERT(trx != nullptr);

  co_return co_await createDocument(&(*trx.get()), definitionName, document,
                                    waitForSync, returnNew);
}

futures::Future<OperationResult> GraphOperations::updateVertex(
    std::string const& collectionName, std::string const& key,
    VPackSlice document, std::optional<RevisionId> rev, bool waitForSync,
    bool returnOld, bool returnNew, bool keepNull) {
  // check if the vertex collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkVertexRes = checkVertexCollectionAvailability(collectionName);
  if (checkVertexRes.fail()) {
    co_return OperationResult(checkVertexRes, options);
  }

  std::vector<std::string> writeCollections;
  writeCollections.emplace_back(collectionName);

  transaction::Options trxOptions;
  trxOptions.waitForSync = waitForSync;
  transaction::Methods trx(ctx(), {}, writeCollections, {}, trxOptions);

  Result tRes = co_await trx.beginAsync();

  if (!tRes.ok()) {
    OperationOptions options(ExecContext::current());
    co_return OperationResult(tRes, options);
  }
  co_return co_await modifyDocument(collectionName, key, document, true,
                                    std::move(rev), waitForSync, returnOld,
                                    returnNew, keepNull, trx);
}

futures::Future<OperationResult> GraphOperations::replaceVertex(
    std::string const& collectionName, std::string const& key,
    VPackSlice document, std::optional<RevisionId> rev, bool waitForSync,
    bool returnOld, bool returnNew, bool keepNull) {
  // check if the vertex collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkVertexRes = checkVertexCollectionAvailability(collectionName);
  if (checkVertexRes.fail()) {
    co_return OperationResult(checkVertexRes, options);
  }

  std::vector<std::string> writeCollections;
  writeCollections.emplace_back(collectionName);

  transaction::Options trxOptions;
  trxOptions.waitForSync = waitForSync;
  transaction::Methods trx(ctx(), {}, writeCollections, {}, trxOptions);

  Result tRes = co_await trx.beginAsync();

  if (!tRes.ok()) {
    OperationOptions options(ExecContext::current());
    co_return OperationResult(tRes, options);
  }
  co_return co_await modifyDocument(collectionName, key, document, false,
                                    std::move(rev), waitForSync, returnOld,
                                    returnNew, keepNull, trx);
}

futures::Future<OperationResult> GraphOperations::createVertex(
    std::string const& collectionName, VPackSlice document, bool waitForSync,
    bool returnNew) {
  // check if the vertex collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkVertexRes = checkVertexCollectionAvailability(collectionName);
  if (checkVertexRes.fail()) {
    co_return OperationResult(checkVertexRes, options);
  }

  transaction::Options trxOptions;

  std::vector<std::string> writeCollections;
  writeCollections.emplace_back(collectionName);
  transaction::Methods trx(ctx(), {}, writeCollections, {}, trxOptions);

  Result res = co_await trx.beginAsync();

  if (!res.ok()) {
    OperationOptions options(ExecContext::current());
    co_return OperationResult(res, options);
  }

  co_return co_await createDocument(&trx, collectionName, document, waitForSync,
                                    returnNew);
}

futures::Future<OperationResult> GraphOperations::removeEdgeOrVertex(
    std::string const& collectionName, std::string const& key,
    std::optional<RevisionId> rev, bool waitForSync, bool returnOld) {
  OperationOptions options;
  options.waitForSync = waitForSync;
  options.returnOld = returnOld;
  options.ignoreRevs = !rev.has_value();

  VPackBufferPtr searchBuffer = _getSearchSlice(key, rev);
  VPackSlice search{searchBuffer->data()};

  // check for used edge definitions in ALL graphs
  GraphManager gmngr{_vocbase, _operationOrigin};

  std::unordered_set<std::string> possibleEdgeCollections;

  auto callback = [&](std::unique_ptr<Graph> graph) -> Result {
    checkForUsedEdgeCollections(*graph, collectionName,
                                possibleEdgeCollections);
    return Result{};
  };
  Result res = gmngr.applyOnAllGraphs(callback);
  if (res.fail()) {
    co_return OperationResult(res, options);
  }

  auto edgeCollections = _graph.edgeCollections();
  std::vector<std::string> trxCollections;

  trxCollections.emplace_back(collectionName);

  CollectionNameResolver resolver{_vocbase};
  for (auto const& it : edgeCollections) {
    trxCollections.emplace_back(it);
    auto col = resolver.getCollection(it);
    if (col && col->isSmart() && col->type() == TRI_COL_TYPE_EDGE) {
      for (auto const& rn : col->realNames()) {
        trxCollections.emplace_back(rn);
      }
    }
  }
  for (auto const& it : possibleEdgeCollections) {
    trxCollections.emplace_back(it);  // add to trx collections
    edgeCollections.emplace(
        it);  // but also to edgeCollections for later iteration
  }

  transaction::Options trxOptions;
  trxOptions.waitForSync = waitForSync;
  transaction::Methods trx{ctx(), {}, trxCollections, {}, trxOptions};
  trx.addHint(transaction::Hints::Hint::GLOBAL_MANAGED);

  res = co_await trx.beginAsync();

  if (!res.ok()) {
    co_return OperationResult(res, options);
  }

  OperationResult result =
      co_await trx.removeAsync(collectionName, search, options);

  {
    aql::QueryString const queryString{
        std::string_view("/*removeEdgeOrVertex*/ FOR e IN @@collection "
                         "FILTER e._from == @toDeleteId "
                         "OR e._to == @toDeleteId "
                         "REMOVE e IN @@collection")};

    std::string const toDeleteId = collectionName + "/" + key;

    for (auto const& edgeCollection : edgeCollections) {
      auto bindVars = std::make_shared<VPackBuilder>();
      bindVars->add(VPackValue(VPackValueType::Object));
      bindVars->add("@collection", VPackValue(edgeCollection));
      bindVars->add("toDeleteId", VPackValue(toDeleteId));
      bindVars->close();

      auto query =
          arangodb::aql::Query::create(ctx(), queryString, std::move(bindVars));
      auto aborter = std::make_shared<aql::QueryAborter>(query);
      auto queryResult = query->executeSync(aborter);

      if (queryResult.result.fail()) {
        co_return OperationResult(std::move(queryResult.result), options);
      }
    }
  }

  res = co_await trx.finishAsync(result.result);

  if (result.ok() && res.fail()) {
    co_return OperationResult(res, options);
  }
  co_return result;
}

futures::Future<OperationResult> GraphOperations::removeVertex(
    std::string const& collectionName, std::string const& key,
    std::optional<RevisionId> rev, bool waitForSync, bool returnOld) {
  // check if the vertex collection is part of the graph
  OperationOptions options(ExecContext::current());
  Result checkVertexRes = checkVertexCollectionAvailability(collectionName);
  if (checkVertexRes.fail()) {
    co_return OperationResult(checkVertexRes, options);
  }
  co_return co_await removeEdgeOrVertex(collectionName, key, rev, waitForSync,
                                        returnOld);
}

bool GraphOperations::collectionExists(std::string const& collection) const {
  GraphManager gmngr{_vocbase, _operationOrigin};
  return gmngr.collectionExists(collection);
}

bool GraphOperations::hasROPermissionsFor(std::string const& collection) const {
  return hasPermissionsFor(collection, auth::Level::RO);
}

bool GraphOperations::hasRWPermissionsFor(std::string const& collection) const {
  return hasPermissionsFor(collection, auth::Level::RW);
}

bool GraphOperations::hasPermissionsFor(std::string const& collection,
                                        auth::Level level) const {
  std::string const& databaseName = _vocbase.name();

  std::stringstream stringstream;
  stringstream << "When checking " << convertFromAuthLevel(level)
               << " permissions for " << databaseName << "." << collection
               << ": ";
  std::string const logprefix = stringstream.str();

  ExecContext const& execContext = ExecContext::current();
  if (!ExecContext::isAuthEnabled()) {
    LOG_TOPIC("08e1f", DEBUG, Logger::GRAPHS)
        << logprefix << "Permissions are turned off.";
    return true;
  }

  if (execContext.canUseCollection(collection, level)) {
    return true;
  }

  LOG_TOPIC("ef8d1", DEBUG, Logger::GRAPHS) << logprefix << "Not allowed.";
  return false;
}

Result GraphOperations::checkEdgeDefinitionPermissions(
    EdgeDefinition const& edgeDefinition) const {
  std::string const& databaseName = _vocbase.name();

  std::stringstream stringstream;
  stringstream << "When checking permissions for edge definition `"
               << edgeDefinition.getName() << "` of graph `" << databaseName
               << "." << graph().name() << "`: ";
  std::string const logprefix = stringstream.str();

  ExecContext const& execContext = ExecContext::current();
  if (!ExecContext::isAuthEnabled()) {
    LOG_TOPIC("18e8e", DEBUG, Logger::GRAPHS)
        << logprefix << "Permissions are turned off.";
    return TRI_ERROR_NO_ERROR;
  }

  // collect all used collections in one container
  std::set<std::string> graphCollections;
  setUnion(graphCollections, edgeDefinition.getFrom());
  setUnion(graphCollections, edgeDefinition.getTo());
  graphCollections.emplace(edgeDefinition.getName());

  bool canUseDatabaseRW = execContext.canUseDatabase(auth::Level::RW);
  for (auto const& col : graphCollections) {
    // We need RO on all collections. And, in case any collection does not
    // exist, we need RW on the database.
    if (!execContext.canUseCollection(col, auth::Level::RO)) {
      LOG_TOPIC("e8a53", DEBUG, Logger::GRAPHS)
          << logprefix << "No read access to " << databaseName << "." << col;
      return TRI_ERROR_FORBIDDEN;
    }
    if (!collectionExists(col) && !canUseDatabaseRW) {
      LOG_TOPIC("2bcf2", DEBUG, Logger::GRAPHS)
          << logprefix << "Creation of " << databaseName << "." << col
          << " is not allowed.";
      return TRI_ERROR_FORBIDDEN;
    }
  }

  return TRI_ERROR_NO_ERROR;
}
