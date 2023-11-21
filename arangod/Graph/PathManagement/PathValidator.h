////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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

#pragma once

#include <velocypack/HashedStringRef.h>
#include "Containers/HashSet.h"
#include "Graph/PathManagement/PathValidatorOptions.h"
#include "Graph/Types/UniquenessLevel.h"
#include "Graph/EdgeDocumentToken.h"
#include "Graph/Types/ValidationResult.h"

#include <velocypack/Builder.h>

namespace arangodb {
namespace aql {
class PruneExpressionEvaluator;
class InputAqlItemRow;
}  // namespace aql
namespace graph {

class ValidationResult;

/*
 * TODO:
 *
 * - EdgeUniqueness, like vertex Uniqueness, and sometimes Edge and Vertex
 * Uniqueness enforce each other. (e.g. => VertexUniqueness == PATH =>
 * EdgeUniquess PATH || NONE.
 *
 * - Prune Condition.
 * - PostFilter Condition.
 * - Path Conditions. Vertex, Edge maybe in LookupInfo
 *     (e.g. p.vertices[*].name ALL == "HANS")
 */
template<class Provider, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
class PathValidator {
  using VertexRef = arangodb::velocypack::HashedStringRef;
  using Step = typename Provider::Step;

 public:
  using ProviderImpl = Provider;
  using PathStoreImpl = PathStore;

  PathValidator(Provider& provider, PathStore& store,
                PathValidatorOptions opts);
  ~PathValidator();

  auto validatePath(typename PathStore::Step& step) -> ValidationResult;
  auto validatePath(typename PathStore::Step const& step,
                    PathValidator<Provider, PathStore, vertexUniqueness,
                                  edgeUniqueness> const& otherValidator)
      -> ValidationResult;

  void reset();

  // Prune section
  [[nodiscard]] bool usesPrune() const;
  void setPruneContext(aql::InputAqlItemRow& inputRow);

  // Post filter section
  [[nodiscard]] bool usesPostFilter() const;
  void setPostFilterContext(aql::InputAqlItemRow& inputRow);

  /**
   * @brief In case prune or postFilter has been enabled, we need to unprepare
   * the context of the inputRow. See explanation in TraversalExecutor.cpp L200
   */
  void unpreparePruneContext();
  void unpreparePostFilterContext();

 private:
  // TODO [GraphRefactor]: const of _store has been removed as it is now
  // necessary to build a PathResult in place. Please double check if we find a
  // better and more elegant solution.
  PathStore& _store;
  Provider& _provider;

  // Only for applied vertex uniqueness
  // TODO: Figure out if we can make this Member template dependent
  //       e.g. std::enable_if<vertexUniqueness != NONE>
  // VertexUniqueness == GLOBAL || PATH => EdgeUniqueness = PATH
  // VertexUniqueness == NONE => EdgeUniqueness == ANY (from user or PATH)
  ::arangodb::containers::HashSet<VertexRef, std::hash<VertexRef>,
                                  std::equal_to<VertexRef>>
      _uniqueVertices;
  ::arangodb::containers::HashSet<
      typename PathStore::Step::EdgeType,
      std::hash<typename PathStore::Step::EdgeType>,
      std::equal_to<typename PathStore::Step::EdgeType>>
      _uniqueEdges;

  PathValidatorOptions _options;
  arangodb::velocypack::Builder _tmpObjectBuilder;

 private:
  [[nodiscard]] auto handleValidationResult(ValidationResult validationResult,
                                            typename PathStore::Step& step)
      -> ValidationResult;
  auto evaluateVertexCondition(typename PathStore::Step const&)
      -> ValidationResult;
  auto evaluateVertexRestriction(typename PathStore::Step const& step) -> bool;

  [[nodiscard]] auto evaluateEdgeCondition(typename PathStore::Step const&)
      -> ValidationResult;

  [[nodiscard]] auto exposeUniqueVertices() const
      -> ::arangodb::containers::HashSet<VertexRef, std::hash<VertexRef>,
                                         std::equal_to<VertexRef>> const&;

  auto evaluateVertexExpression(arangodb::aql::Expression* expression,
                                arangodb::velocypack::Slice value) -> bool;

  auto evaluateEdgeExpression(arangodb::aql::Expression* expression,
                              arangodb::velocypack::Slice value) -> bool;

  auto checkValidDisjointPath(typename PathStore::Step const& lastStep)
      -> arangodb::graph::ValidationResult::Type;

  auto isDisjoint() const { return _options.isDisjoint(); }
  auto isSatelliteLeader() const {
    if (_options.isClusterOneShardRuleEnabled()) {
      // In case the cluster one shard rule is enabled, we will declare
      // any shard as "a leader" - which means it can be used and in this
      // particular case, we are sure that we cannot produce duplicate results
      return true;
    }
    return _options.isSatelliteLeader();
  }
};
}  // namespace graph
}  // namespace arangodb
