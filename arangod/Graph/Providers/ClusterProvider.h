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

#pragma once

#include "Graph/EdgeDocumentToken.h"
#include "Graph/Providers/BaseProviderOptions.h"
#include "Graph/Providers/BaseStep.h"
#include "Graph/Providers/TypeAliases.h"

#include "Aql/TraversalStats.h"
#include "Basics/ResourceUsage.h"
#include "Basics/StringHeap.h"
#include "Containers/FlatHashMap.h"

#include "Graph/Steps/ClusterProviderStep.h"

#include <vector>

namespace arangodb {

namespace futures {
template<typename T>
class Future;
}

namespace aql {
class QueryContext;
}

namespace velocypack {
class Builder;
class HashedStringRef;
}  // namespace velocypack

namespace transaction {
class Methods;
}
namespace graph {

// TODO: we need to control from the outside if and which parts of the vertex -
// (will be implemented in the future via template parameters) data should be
// returned THis is most-likely done via Template Parameter like this:
// template<ProduceVertexData>
template<class StepImpl>
class ClusterProvider {
 public:
  using Options = ClusterBaseProviderOptions;
  using Step = StepImpl;

 public:
  ClusterProvider(arangodb::aql::QueryContext& queryContext,
                  ClusterBaseProviderOptions opts,
                  arangodb::ResourceMonitor& resourceMonitor);
  ClusterProvider(ClusterProvider const&) = delete;
  ClusterProvider(ClusterProvider&&) = default;
  ~ClusterProvider();

  ClusterProvider& operator=(ClusterProvider const&) = delete;

  void clear();
  void clearWithForce();

  auto startVertex(const VertexType& vertex, size_t depth = 0,
                   double weight = 0.0) -> Step;
  auto fetch(std::vector<Step*> const& looseEnds)
      -> futures::Future<std::vector<Step*>>;
  auto fetchVertices(std::vector<Step*> const& looseEnds) -> std::vector<Step*>;
  auto fetchEdges(const std::vector<Step*>& fetchedVertices) -> Result;
  auto expand(Step const& from, size_t previous,
              std::function<void(Step)> const& callback) -> void;

  void addVertexToBuilder(typename Step::Vertex const& vertex,
                          arangodb::velocypack::Builder& builder);
  void addEdgeToBuilder(typename Step::Edge const& edge,
                        arangodb::velocypack::Builder& builder);

  VPackSlice readEdge(EdgeType const& edgeID);

  void addEdgeIDToBuilder(typename Step::Edge const& edge,
                          arangodb::velocypack::Builder& builder);

  void addEdgeToLookupMap(typename Step::Edge const& edge,
                          arangodb::velocypack::Builder& builder);

  auto getEdgeId(typename Step::Edge const& edge) -> std::string;

  auto getEdgeIdRef(typename Step::Edge const& edge) -> EdgeType;

  // fetch vertices and store in cache
  auto fetchVerticesFromEngines(std::vector<Step*> const& looseEnds,
                                std::vector<Step*>& result) -> void;

  // fetch edges and store in cache
  auto fetchEdgesFromEngines(std::vector<Step*> const& step) -> Result;

  void destroyEngines();

  [[nodiscard]] transaction::Methods* trx();
  [[nodiscard]] TRI_vocbase_t const& vocbase() const;

  void prepareIndexExpressions(aql::Ast* ast);

  aql::TraversalStats stealStats();

  void prepareContext(aql::InputAqlItemRow input);
  void unPrepareContext();
  bool isResponsible(Step const& step) const;

  [[nodiscard]] bool hasDepthSpecificLookup(uint64_t depth) const noexcept;

 private:
  // Unique_ptr to have this class movable, and to keep reference of trx()
  // alive - Note: _trx must be first here because it is used in _cursor
  std::unique_ptr<arangodb::transaction::Methods> _trx;

  arangodb::aql::QueryContext* _query;

  arangodb::ResourceMonitor* _resourceMonitor;

  ClusterBaseProviderOptions _opts;

  arangodb::aql::TraversalStats _stats;

  /// @brief vertex reference to all connected edges including the edges target
  // Info: SourceVertex -> [[ConnectedEdge, TargetVertex], ...]
  containers::FlatHashMap<VertexType,
                          std::vector<std::pair<EdgeType, VertexType>>>
      _vertexConnectedEdges;
};
}  // namespace graph
}  // namespace arangodb
