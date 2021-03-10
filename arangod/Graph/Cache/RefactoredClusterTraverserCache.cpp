////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#include "RefactoredClusterTraverserCache.h"

#include "Aql/AqlValue.h"
#include "Aql/Query.h"
#include "Graph/BaseOptions.h"
#include "Graph/EdgeDocumentToken.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::graph;

namespace {
constexpr size_t costPerPersistedString = sizeof(void*) + sizeof(arangodb::velocypack::HashedStringRef);
constexpr size_t heapBlockSize = 4096;
};

RefactoredClusterTraverserCache::RefactoredClusterTraverserCache(
    std::unordered_map<ServerID, aql::EngineId> const* engines, ResourceMonitor& resourceMonitor)
    : _resourceMonitor{resourceMonitor},
      _stringHeap(resourceMonitor, heapBlockSize), /* arbitrary block-size may be adjusted for performance */
      _datalake(resourceMonitor),
      _engines(engines) {}

RefactoredClusterTraverserCache::~RefactoredClusterTraverserCache() {
  clear();
}

void RefactoredClusterTraverserCache::clear() {
  _resourceMonitor.decreaseMemoryUsage(_persistedStrings.size() * ::costPerPersistedString);
  _stringHeap.clear();
  _persistedStrings.clear();
}

auto RefactoredClusterTraverserCache::cacheVertex(VertexType const& vertexId,
                                                  velocypack::Slice vertexSlice) -> void {
  _vertexData.try_emplace(vertexId, vertexSlice);
}

namespace {
VertexType getEdgeDestination(arangodb::velocypack::Slice edge, VertexType const& origin) {
  if (edge.isString()) {
    return VertexType{edge};
  }

  TRI_ASSERT(edge.isObject());
  auto from = edge.get(arangodb::StaticStrings::FromString);
  TRI_ASSERT(from.isString());
  if (from.stringRef() == origin.stringRef()) {
    auto to = edge.get(arangodb::StaticStrings::ToString);
    TRI_ASSERT(to.isString());
    return VertexType{to};
  }
  return VertexType{from};
}
}  // namespace

auto RefactoredClusterTraverserCache::cacheEdge(VertexType origin, EdgeType edgeId,
                                                velocypack::Slice edgeSlice,
                                                bool backward) -> void {
  TRI_ASSERT(isVertexCached(origin));

  auto edgeToEmplace =
      std::pair{edgeId, VertexType{getEdgeDestination(edgeSlice, origin)}};

  if (backward) {
    _edgeDataBackward.try_emplace(edgeId, edgeSlice);
    auto& edgeVectorBackward = _vertexConnectedEdgesBackward[origin];
    edgeVectorBackward.emplace_back(std::move(edgeToEmplace));
  } else {
    _edgeDataForward.try_emplace(edgeId, edgeSlice);
    auto& edgeVectorForward = _vertexConnectedEdgesForward[origin];
    edgeVectorForward.emplace_back(std::move(edgeToEmplace));
  }
}

auto RefactoredClusterTraverserCache::getVertexRelations(VertexType const& vertex, bool backward)
    -> std::vector<std::pair<EdgeType, VertexType>> const& {
  TRI_ASSERT(isVertexCached(vertex));

  if (backward) {
    return _vertexConnectedEdgesBackward[vertex];
  }

  return _vertexConnectedEdgesForward[vertex];
}

auto RefactoredClusterTraverserCache::isVertexCached(VertexType const& vertexKey) const
    -> bool {
  return _vertexData.find(vertexKey) != _vertexData.end();
}

auto RefactoredClusterTraverserCache::isEdgeCached(EdgeType const& edgeKey,
                                                   bool backward) const -> bool {
  if (backward) {
    return _edgeDataBackward.find(edgeKey) != _edgeDataBackward.end();
  }
  return _edgeDataForward.find(edgeKey) != _edgeDataForward.end();
}

auto RefactoredClusterTraverserCache::getCachedVertex(VertexType const& vertex) const -> VPackSlice {
  if (!isVertexCached(vertex)) {
    return VPackSlice::noneSlice();
  }
  return _vertexData.at(vertex);
}

auto RefactoredClusterTraverserCache::getCachedEdge(EdgeType const& edge, bool backward) const
    -> VPackSlice {
  if (!isEdgeCached(edge, backward)) {
    return VPackSlice::noneSlice();
  }
  if (backward) {
    return _edgeDataBackward.at(edge);
  }
  return _edgeDataForward.at(edge);
}

auto RefactoredClusterTraverserCache::persistString(arangodb::velocypack::HashedStringRef idString) -> arangodb::velocypack::HashedStringRef {
  auto it = _persistedStrings.find(idString);
  if (it != _persistedStrings.end()) {
    return *it;
  }
  auto res = _stringHeap.registerString(idString);
  {
    ResourceUsageScope guard(_resourceMonitor, ::costPerPersistedString);
   
    _persistedStrings.emplace(res);
    
    // now make the TraverserCache responsible for memory tracking
    guard.steal();
  }
  return res;
}
