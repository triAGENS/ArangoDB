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

#pragma once

#include <velocypack/HashedStringRef.h>
#include "Containers/HashSet.h"

// TODO Temporary include
#include "Graph/Enumerators/OneSidedEnumeratorInterface.h"
#include "Graph/Providers/TypeAliases.h"
#include "Graph/TraverserOptions.h"

#include <numeric>
#include <unordered_map>

namespace arangodb {

namespace velocypack {
class Builder;
}  // namespace velocypack

namespace graph {

template <class ProviderType, class PathStoreType, class Step>
class SingleProviderPathResult : public PathResultInterface {
  using VertexRef = arangodb::velocypack::HashedStringRef;

 public:
  SingleProviderPathResult(Step* step, ProviderType& provider, PathStoreType& store);
  auto clear() -> void;
  auto appendVertex(typename Step::Vertex v) -> void;
  auto prependVertex(typename Step::Vertex v) -> void;
  auto appendEdge(typename Step::Edge e) -> void;
  auto prependEdge(typename Step::Edge e) -> void;

  auto toVelocyPack(arangodb::velocypack::Builder& builder) -> void override;

  /**
   * @brief Appends this path as a SchreierVector entry into the given builder
   */
  auto writeSmartGraphDFSResult(arangodb::velocypack::Builder& builder,
                                size_t& currentLength) -> void override;

  /**
   * @brief Builds a lookup table which can be used to populate a builder later
   */
  auto writeSmartGraphBFSResult(
      std::unordered_map<VertexType, std::vector<std::unique_ptr<PathResultInterface>>>& bfsLookupTable,
      size_t& bfsCurrentDepth) -> void override;

  auto isEmpty() const -> bool;

 private:
  Step* _step;

  std::vector<typename Step::Vertex> _vertices;
  std::vector<typename Step::Edge> _edges;

  // Provider for the path
  ProviderType& _provider;
  PathStoreType& _store;
};
}  // namespace graph
}  // namespace arangodb
