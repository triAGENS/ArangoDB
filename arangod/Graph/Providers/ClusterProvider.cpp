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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "ClusterProvider.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/InputAqlItemRow.h"
#include "Aql/QueryContext.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Futures/Future.h"
#include "Futures/Utilities.h"
#include "Logger/LogMacros.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Transaction/Helpers.h"
#include "VocBase/vocbase.h"

#include <utility>
#include <vector>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::futures;
using namespace arangodb::graph;

using Helper = arangodb::basics::VelocyPackHelper;

namespace {
constexpr size_t costPerVertexOrEdgeType =
    sizeof(arangodb::velocypack::HashedStringRef);

std::string const edgeUrl = "/_internal/traverser/edge/";
std::string const vertexUrl = "/_internal/traverser/vertex/";

VertexType getEdgeDestination(arangodb::velocypack::Slice edge,
                              VertexType const& origin) {
  if (edge.isString()) {
    return VertexType{edge};
  }

  TRI_ASSERT(edge.isObject());
  auto from = edge.get(arangodb::StaticStrings::FromString);
  TRI_ASSERT(from.isString());
  if (from.stringView() == origin.stringView()) {
    auto to = edge.get(arangodb::StaticStrings::ToString);
    TRI_ASSERT(to.isString());
    return VertexType{to};
  }
  return VertexType{from};
}

ClusterProviderStep::FetchedType getFetchedType(bool vertexFetched,
                                                bool edgesFetched) {
  if (vertexFetched) {
    if (edgesFetched) {
      return ClusterProviderStep::FetchedType::VERTEX_AND_EDGES_FETCHED;
    } else {
      return ClusterProviderStep::FetchedType::VERTEX_FETCHED;
    }
  } else {
    if (edgesFetched) {
      return ClusterProviderStep::FetchedType::EDGES_FETCHED;
    } else {
      return ClusterProviderStep::FetchedType::UNFETCHED;
    }
  }
}

}  // namespace

template<class StepImpl>
ClusterProvider<StepImpl>::ClusterProvider(
    arangodb::aql::QueryContext& queryContext, ClusterBaseProviderOptions opts,
    arangodb::ResourceMonitor& resourceMonitor)
    : _trx(std::make_unique<arangodb::transaction::Methods>(
          queryContext.newTrxContext())),
      _query(&queryContext),
      _resourceMonitor(&resourceMonitor),
      _opts(std::move(opts)),
      _stats{} {}

template<class StepImpl>
ClusterProvider<StepImpl>::~ClusterProvider() {
  clearWithForce();  // Make sure we actually free all memory in the edge cache!
}

template<class StepImpl>
void ClusterProvider<StepImpl>::clear() {
  if (_opts.clearEdgeCacheOnClear()) {
    clearWithForce();
  }
}

template<class StepImpl>
void ClusterProvider<StepImpl>::clearWithForce() {
  for (auto const& entry : _vertexConnectedEdges) {
    _resourceMonitor->decreaseMemoryUsage(
        costPerVertexOrEdgeType +
        (entry.second.size() * (costPerVertexOrEdgeType * 2)));
  }
  _vertexConnectedEdges.clear();
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::startVertex(const VertexType& vertex,
                                            size_t depth, double weight)
    -> Step {
  LOG_TOPIC("da308", TRACE, Logger::GRAPHS)
      << "<ClusterProvider> Start Vertex:" << vertex;
  // Create the default initial step.
  TRI_ASSERT(weight == 0.0);  // Not implemented yet
  return Step{_opts.getCache()->persistString(vertex), depth, weight};
}

template<class StepImpl>
void ClusterProvider<StepImpl>::fetchVerticesFromEngines(
    std::vector<Step*> const& looseEnds, std::vector<Step*>& result) {
  // slow path, sharding not deducable from _id
  bool mustSend = false;

  transaction::BuilderLeaser leased(trx());
  leased->openObject();

  if (_opts.produceVertices()) {
    // slow path, sharding not deducable from _id
    leased->add("keys", VPackValue(VPackValueType::Array));
    for (auto const& looseEnd : looseEnds) {
      TRI_ASSERT(looseEnd->isLooseEnd());
      auto const& vertexId = looseEnd->getVertex().getID();
      if (!_opts.getCache()->isVertexCached(vertexId)) {
        leased->add(VPackValuePair(vertexId.data(), vertexId.size(),
                                   VPackValueType::String));
        mustSend = true;
        LOG_TOPIC("9e0f4", TRACE, Logger::GRAPHS)
            << "<ClusterProvider> Fetching vertex " << vertexId;
      }
    }
    leased->close();  // 'keys' Array
  }
  leased->close();  // base object

  if (!mustSend) {
    // nothing to send. save requests...
    for (auto& looseEnd : looseEnds) {
      auto const& vertexId = looseEnd->getVertex().getID();
      if (!_opts.getCache()->isVertexCached(vertexId)) {
        _opts.getCache()->cacheVertex(vertexId, VPackSlice::nullSlice());
      }
      result.emplace_back(looseEnd);
      looseEnd->setVertexFetched();
    }
    return;
  }

  auto* pool =
      trx()->vocbase().server().template getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.database = trx()->vocbase().name();
  reqOpts.skipScheduler = true;  // hack to avoid scheduler queue

  auto const* engines = _opts.engines();
  std::vector<Future<network::Response>> futures;
  futures.reserve(engines->size());

  ScopeGuard sg([&]() noexcept {
    for (Future<network::Response>& f : futures) {
      try {
        // TODO: As soon as we switch to the new future library, we need to
        // replace the wait with proper *finally* method.
        f.wait();
      } catch (...) {
      }
    }
  });

  for (auto const& engine : *engines) {
    futures.emplace_back(network::sendRequestRetry(
        pool, "server:" + engine.first, fuerte::RestVerb::Put,
        ::vertexUrl + StringUtils::itoa(engine.second), leased->bufferRef(),
        reqOpts));
  }

  for (Future<network::Response>& f : futures) {
    // NOTE: If you remove this waitAndGet() in favour of an asynchronous
    // operation, remember to remove the `skipScheduler = true` option of the
    // corresponding requests.
    network::Response const& r = f.waitAndGet();

    if (r.fail()) {
      THROW_ARANGO_EXCEPTION(network::fuerteToArangoErrorCode(r));
    }

    auto payload = r.response().stealPayload();
    bool needToRetainPayload = false;

    VPackSlice resSlice(payload->data());
    if (!resSlice.isObject()) {
      // Response has invalid format
      THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_CORRUPTED_JSON);
    }
    if (r.statusCode() != fuerte::StatusOK) {
      // We have an error case here. Throw it.
      THROW_ARANGO_EXCEPTION(
          network::resultFromBody(resSlice, TRI_ERROR_INTERNAL));
    }

    for (auto pair : VPackObjectIterator(resSlice, /*sequential*/ true)) {
      VertexType vertexKey(pair.key);

      if (!_opts.getCache()->isVertexCached(vertexKey)) {
        // Will be protected by the datalake.
        // We flag to retain the payload.
        _opts.getCache()->cacheVertex(vertexKey, pair.value);
        // increase scanned Index for every vertex we cache.
        _stats.incrScannedIndex(1);
        needToRetainPayload = true;
      }
    }

    if (needToRetainPayload) {
      // We have stored at least one entry from this payload.
      // Retain it.
      _opts.getCache()->datalake().add(std::move(payload));
    }
  }

  // Note: This disables the ScopeGuard
  futures.clear();

  // put back all looseEnds. We were able to cache
  for (auto& looseEnd : looseEnds) {
    auto const& vertexId = looseEnd->getVertex().getID();
    if (!_opts.getCache()->isVertexCached(vertexId)) {
      // if we end up here. We were not able to cache the requested vertex
      // (e.g. it does not exist)
      _query->warnings().registerWarning(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND,
                                         vertexId.toString());
      _opts.getCache()->cacheVertex(vertexId, VPackSlice::nullSlice());
    }
    result.emplace_back(looseEnd);
    looseEnd->setVertexFetched();
  }

  _stats.incrHttpRequests(_opts.engines()->size());
}

template<class StepImpl>
void ClusterProvider<StepImpl>::destroyEngines() {
  if (!ServerState::instance()->isCoordinator()) {
    return;
  }

  auto* pool =
      trx()->vocbase().server().template getFeature<NetworkFeature>().pool();
  // We have to clean up the engines in Coordinator Case.
  if (pool == nullptr) {
    return;
  }
  // nullptr only happens on controlled server shutdown

  network::RequestOptions options;
  options.database = trx()->vocbase().name();
  options.timeout = network::Timeout(30.0);
  options.skipScheduler = true;  // hack to speed up future.get()

  auto const* engines = _opts.engines();
  for (auto const& engine : *engines) {
    _stats.incrHttpRequests(1);

    // NOTE: If you remove this waitAndGet() in favour of an asynchronous
    // operation, remember to remove the `skipScheduler = true` option of the
    // corresponding requests.
    auto res = network::sendRequestRetry(
                   pool, "server:" + engine.first, fuerte::RestVerb::Delete,
                   "/_internal/traverser/" +
                       arangodb::basics::StringUtils::itoa(engine.second),
                   VPackBuffer<uint8_t>(), options)
                   .waitAndGet();

    if (res.error != fuerte::Error::NoError) {
      // Note If there was an error on server side we do not have
      // CL_COMM_SENT
      LOG_TOPIC("d31a5", ERR, arangodb::Logger::GRAPHS)
          << "Could not destroy all traversal engines: "
          << TRI_errno_string(network::fuerteToArangoErrorCode(res));
    }
  }
}

template<class StepImpl>
Result ClusterProvider<StepImpl>::fetchEdgesFromEngines(Step* step) {
  TRI_ASSERT(step != nullptr);
  LOG_TOPIC("fa7dc", TRACE, Logger::GRAPHS)
      << "<ClusterProvider> Expanding " << step->getVertex().getID();
  auto const* engines = _opts.engines();
  transaction::BuilderLeaser leased(trx());
  leased->openObject(true);
  leased->add("backward",
              VPackValue(_opts.isBackward()));  // [GraphRefactor] ksp only?

  // [GraphRefactor] TODO: Differentiate between algorithms -> traversal vs.
  // ksp.
  /* Needed for TRAVERSALS only - Begin */
  // But note that the field "depth" must be set to an integer in any case!
  leased->add("depth", VPackValue(step->getDepth()));
  if (_opts.expressionContext() != nullptr) {
    leased->add(VPackValue("variables"));
    leased->openArray();
    _opts.expressionContext()->serializeAllVariables(trx()->vpackOptions(),
                                                     *(leased.get()));
    leased->close();
  }
  /* Needed for TRAVERSALS only - End */

  leased->add("keys", VPackValue(step->getVertex().getID().toString()));
  leased->close();

  auto* pool =
      trx()->vocbase().server().template getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.database = trx()->vocbase().name();
  reqOpts.skipScheduler = true;  // hack to avoid scheduler queue

  std::vector<Future<network::Response>> futures;
  futures.reserve(engines->size());

  ScopeGuard sg([&]() noexcept {
    for (Future<network::Response>& f : futures) {
      try {
        // TODO: As soon as we switch to the new future library, we need to
        // replace the wait with proper *finally* method.
        f.wait();
      } catch (...) {
      }
    }
  });

  for (auto const& engine : *engines) {
    futures.emplace_back(network::sendRequestRetry(
        pool, "server:" + engine.first, fuerte::RestVerb::Put,
        ::edgeUrl + StringUtils::itoa(engine.second), leased->bufferRef(),
        reqOpts));
  }

  std::vector<std::pair<EdgeType, VertexType>> connectedEdges;
  for (Future<network::Response>& f : futures) {
    // NOTE: If you remove this waitAndGet() in favour of an asynchronous
    // operation, remember to remove the `skipScheduler = true` option of the
    // corresponding requests.
    network::Response const& r = f.waitAndGet();

    if (r.fail()) {
      return network::fuerteToArangoErrorCode(r);
    }

    auto payload = r.response().stealPayload();
    VPackSlice resSlice(payload->data());
    if (!resSlice.isObject()) {
      // Response has invalid format
      return TRI_ERROR_HTTP_CORRUPTED_JSON;
    }
    Result res = network::resultFromBody(resSlice, TRI_ERROR_NO_ERROR);
    if (res.fail()) {
      return res;
    }
    _stats.incrScannedIndex(
        Helper::getNumericValue<size_t>(resSlice, "readIndex", 0));
    _stats.incrFiltered(
        Helper::getNumericValue<size_t>(resSlice, "filtered", 0));
    _stats.incrCursorsCreated(
        Helper::getNumericValue<size_t>(resSlice, "cursorsCreated", 0));
    _stats.incrCursorsRearmed(
        Helper::getNumericValue<size_t>(resSlice, "cursorsRearmed", 0));
    _stats.incrCacheHits(
        Helper::getNumericValue<size_t>(resSlice, "cacheHits", 0));
    _stats.incrCacheMisses(
        Helper::getNumericValue<size_t>(resSlice, "cacheMisses", 0));

    bool allCached = true;
    VPackSlice edges = resSlice.get("edges");
    for (VPackSlice e : VPackArrayIterator(edges)) {
      VPackSlice id = e.get(StaticStrings::IdString);
      if (!id.isString()) {
        // invalid id type
        LOG_TOPIC("eb7cd", ERR, Logger::GRAPHS)
            << "got invalid edge id type: " << id.typeName();
        continue;
      }
      LOG_TOPIC("f4b3b", TRACE, Logger::GRAPHS)
          << "<ClusterProvider> Neighbor of " << step->getVertex().getID()
          << " -> " << id.toJson();

      auto [edge, needToCache] = _opts.getCache()->persistEdgeData(e);
      if (needToCache) {
        allCached = false;
      }

      arangodb::velocypack::HashedStringRef edgeIdRef(
          edge.get(StaticStrings::IdString));

      auto edgeToEmplace = std::make_pair(
          edgeIdRef,
          VertexType{getEdgeDestination(edge, step->getVertex().getID())});

      connectedEdges.emplace_back(edgeToEmplace);
    }

    if (!allCached) {
      _opts.getCache()->datalake().add(std::move(payload));
    }
  }
  // Note: This disables the ScopeGuard
  futures.clear();

  std::uint64_t memoryPerItem =
      costPerVertexOrEdgeType +
      (connectedEdges.size() * (costPerVertexOrEdgeType * 2));
  ResourceUsageScope guard(*_resourceMonitor, memoryPerItem);

  auto [it, inserted] = _vertexConnectedEdges.emplace(
      step->getVertex().getID(), std::move(connectedEdges));
  if (inserted) {
    guard.steal();
  }

  return TRI_ERROR_NO_ERROR;
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::fetchVertices(
    std::vector<Step*> const& looseEnds) -> std::vector<Step*> {
  std::vector<Step*> result{};

  if (!looseEnds.empty()) {
    result.reserve(looseEnds.size());
    if (!_opts.produceVertices()) {
      // in that case we do not have to fetch the actual vertex data

      for (auto& lE : looseEnds) {
        if (!_opts.getCache()->isVertexCached(lE->getVertexIdentifier())) {
          // we'll only cache the vertex id, we do not need the data
          _opts.getCache()->cacheVertex(lE->getVertexIdentifier(),
                                        VPackSlice::nullSlice());
        }
        result.emplace_back(lE);
        lE->setVertexFetched();
      }
    } else {
      fetchVerticesFromEngines(looseEnds, result);
    }
  }
  return result;
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::fetchEdges(
    std::vector<Step*> const& fetchedVertices) -> Result {
  for (auto const& step : fetchedVertices) {
    if (!_vertexConnectedEdges.contains(step->getVertex().getID())) {
      auto res = fetchEdgesFromEngines(step);
      _stats.incrHttpRequests(_opts.engines()->size());

      if (res.fail()) {
        THROW_ARANGO_EXCEPTION(res);
      }
    }
    // else: We already fetched this vertex.

    // mark a looseEnd as fetched as vertex fetch + edges fetch was a success
    step->setEdgesFetched();
  }
  return TRI_ERROR_NO_ERROR;
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::fetch(std::vector<Step*> const& looseEnds)
    -> futures::Future<std::vector<Step*>> {
  LOG_TOPIC("03c1b", TRACE, Logger::GRAPHS) << "<ClusterProvider> Fetching...";
  std::vector<Step*> result = fetchVertices(looseEnds);
  fetchEdges(result);
  return futures::makeFuture(std::move(result));
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::expand(
    Step const& step, size_t previous,
    std::function<void(Step)> const& callback) -> void {
  TRI_ASSERT(!step.isLooseEnd());
  auto const& vertex = step.getVertex();

  TRI_ASSERT(_opts.getCache()->isVertexCached(vertex.getID()));
  auto const relations = _vertexConnectedEdges.find(vertex.getID());
  TRI_ASSERT(relations != _vertexConnectedEdges.end());

  if (ADB_LIKELY(relations != _vertexConnectedEdges.end())) {
    for (auto const& relation : relations->second) {
      bool vertexCached = _opts.getCache()->isVertexCached(relation.second);
      bool edgesCached = _vertexConnectedEdges.contains(relation.second);
      typename Step::FetchedType fetchedType =
          ::getFetchedType(vertexCached, edgesCached);
      // [GraphRefactor] TODO: KShortestPaths does not require Depth/Weight. We
      // need a mechanism here as well to distinguish between (non)required
      // parameters.
      callback(
          Step{relation.second, relation.first, previous, fetchedType,
               step.getDepth() + 1,
               _opts.weightEdge(step.getWeight(), readEdge(relation.first))});
    }
  } else {
    throw std::out_of_range{"ClusterProvider::_vertexConnectedEdges"};
  }
}

template<class StepImpl>
void ClusterProvider<StepImpl>::addVertexToBuilder(
    typename Step::Vertex const& vertex,
    arangodb::velocypack::Builder& builder) {
  TRI_ASSERT(_opts.getCache()->isVertexCached(vertex.getID()));
  builder.add(_opts.getCache()->getCachedVertex(vertex.getID()));
};

template<class StepImpl>
auto ClusterProvider<StepImpl>::addEdgeToBuilder(
    typename Step::Edge const& edge, arangodb::velocypack::Builder& builder)
    -> void {
  builder.add(_opts.getCache()->getCachedEdge(edge.getID()));
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::addEdgeIDToBuilder(
    typename Step::Edge const& edge, arangodb::velocypack::Builder& builder)
    -> void {
  builder.add(VPackValue(edge.getID().begin()));
}

template<class StepImpl>
void ClusterProvider<StepImpl>::addEdgeToLookupMap(
    typename Step::Edge const& edge, arangodb::velocypack::Builder& builder) {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(VPackValue(edge.getID().begin()));
  builder.add(_opts.getCache()->getCachedEdge(edge.getID()));
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::getEdgeId(typename Step::Edge const& edge)
    -> std::string {
  return edge.getID().toString();
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::getEdgeIdRef(typename Step::Edge const& edge)
    -> EdgeType {
  return edge.getID();
}

template<class StepImpl>
auto ClusterProvider<StepImpl>::readEdge(EdgeType const& edgeID) -> VPackSlice {
  return _opts.getCache()->getCachedEdge(edgeID);
}

template<class StepImpl>
void ClusterProvider<StepImpl>::prepareIndexExpressions(aql::Ast* ast) {
  // Nothing to do here. The variables are send over in a different way.
  // We do not make use of special indexes here anyways.
}

template<class StepImpl>
arangodb::transaction::Methods* ClusterProvider<StepImpl>::trx() {
  return _trx.get();
}

template<class Step>
TRI_vocbase_t const& ClusterProvider<Step>::vocbase() const {
  TRI_ASSERT(_trx != nullptr);
  TRI_ASSERT(_trx->state() != nullptr);
  TRI_ASSERT(_trx->transactionContextPtr() != nullptr);
  return _trx.get()->vocbase();
}

template<class StepImpl>
arangodb::aql::TraversalStats ClusterProvider<StepImpl>::stealStats() {
  auto t = _stats;
  _stats.clear();
  return t;
}

template<class StepImpl>
void ClusterProvider<StepImpl>::prepareContext(aql::InputAqlItemRow input) {
  _opts.prepareContext(std::move(input));
}

template<class StepImpl>
void ClusterProvider<StepImpl>::unPrepareContext() {
  _opts.unPrepareContext();
}

template<class StepImpl>
bool ClusterProvider<StepImpl>::isResponsible(StepImpl const& step) const {
  return true;
}

template<class StepImpl>
bool ClusterProvider<StepImpl>::hasDepthSpecificLookup(
    uint64_t depth) const noexcept {
  return _opts.hasDepthSpecificLookup(depth);
}
template class graph::ClusterProvider<ClusterProviderStep>;
