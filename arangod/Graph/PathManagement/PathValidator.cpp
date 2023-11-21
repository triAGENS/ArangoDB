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

#include "PathValidator.h"
#include "Aql/AstNode.h"
#include "Aql/PruneExpressionEvaluator.h"
#include "Basics/ScopeGuard.h"
#include "Graph/PathManagement/PathStore.h"
#include "Graph/PathManagement/PathStoreTracer.h"
#include "Graph/Providers/ClusterProvider.h"
#include "Graph/Providers/ProviderTracer.h"
#include "Graph/Providers/SingleServerProvider.h"
#include "Graph/PathManagement/SingleProviderPathResult.h"
#include "Graph/Steps/SingleServerProviderStep.h"
#include "Graph/Types/ValidationResult.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/Graph/Steps/SmartGraphStep.h"
#include "Enterprise/Graph/Providers/SmartGraphProvider.h"

// For additional information, please read PathValidatorEE.cpp
#include "Enterprise/Graph/PathValidatorEE.cpp"
#endif

#include "Basics/Exceptions.h"

using namespace arangodb;
using namespace arangodb::graph;

template<class Provider, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
PathValidator<Provider, PathStore, vertexUniqueness,
              edgeUniqueness>::PathValidator(Provider& provider,
                                             PathStore& store,
                                             PathValidatorOptions opts)
    : _store(store), _provider(provider), _options(std::move(opts)) {}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
PathValidator<ProviderType, PathStore, vertexUniqueness,
              edgeUniqueness>::~PathValidator() = default;

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::validatePath(typename PathStore::Step& step)
    -> ValidationResult {
  auto res = evaluateVertexCondition(step);
  if (res.isFiltered() && res.isPruned()) {
    // Can give up here. This Value is not used
    return handleValidationResult(res, step);
  }

#ifdef USE_ENTERPRISE
  if (isDisjoint()) {
    auto validDisjPathRes = checkValidDisjointPath(step);
    if (validDisjPathRes == ValidationResult::Type::FILTER_AND_PRUNE ||
        validDisjPathRes == ValidationResult::Type::FILTER) {
      res.combine(validDisjPathRes);
      return handleValidationResult(res, step);
    }
  }
#endif

  if constexpr (vertexUniqueness == VertexUniquenessLevel::PATH) {
    reset();
    // Reserving here is pointless, we will test paths that increase by at most
    // 1 entry.

    bool success = _store.visitReversePath(
        step, [&](typename PathStore::Step const& step) -> bool {
          auto const& [unusedV, addedVertex] =
              _uniqueVertices.emplace(step.getVertexIdentifier());

          // If this add fails, we need to exclude this path
          return addedVertex;
        });
    if (!success) {
      res.combine(ValidationResult::Type::FILTER_AND_PRUNE);
    }
  }
  if constexpr (vertexUniqueness == VertexUniquenessLevel::GLOBAL) {
    // In case we have VertexUniquenessLevel::GLOBAL, we do not have to take
    // care about the EdgeUniquenessLevel.
    auto const& [unusedV, addedVertex] =
        _uniqueVertices.emplace(step.getVertexIdentifier());
    // If this add fails, we need to exclude this path
    if (!addedVertex) {
      res.combine(ValidationResult::Type::FILTER_AND_PRUNE);
    }
  }
  if constexpr (vertexUniqueness == VertexUniquenessLevel::NONE &&
                edgeUniqueness == EdgeUniquenessLevel::PATH) {
    if (step.getDepth() > 1) {
      reset();

      bool edgeSuccess = _store.visitReversePath(
          step, [&](typename PathStore::Step const& step) -> bool {
            if (step.isFirst()) {
              return true;
            }

            auto const& [unusedE, addedEdge] =
                _uniqueEdges.emplace(step.getEdgeIdentifier());
            // If this add fails, we need to exclude this path
            return addedEdge;
          });

      // If this add fails, we need to exclude this path
      if (!edgeSuccess) {
        res.combine(ValidationResult::Type::FILTER_AND_PRUNE);
      }
    }
  }
  return handleValidationResult(res, step);
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    validatePath(typename PathStore::Step const& step,
                 PathValidator<ProviderType, PathStore, vertexUniqueness,
                               edgeUniqueness> const& otherValidator)
        -> ValidationResult {
  if constexpr (vertexUniqueness == VertexUniquenessLevel::PATH) {
    // For PATH: take _uniqueVertices of otherValidator, and run Visitor of
    // other side, check if one vertex is duplicate.
    auto const& otherUniqueVertices = otherValidator.exposeUniqueVertices();

    bool success = _store.visitReversePath(
        step, [&](typename PathStore::Step const& innerStep) -> bool {
          // compare memory address for equality (instead of comparing their
          // values)
          if (&step == &innerStep) {
            return true;
          }

          // If otherUniqueVertices has our step, we will return false and
          // abort. Otherwise we'll return true here. This guarantees we have no
          // vertex on both sides of the path twice.
          return otherUniqueVertices.find(innerStep.getVertexIdentifier()) ==
                 otherUniqueVertices.end();
        });
    if (!success) {
      return ValidationResult{ValidationResult::Type::FILTER_AND_PRUNE};
    }
    return ValidationResult{ValidationResult::Type::TAKE};
  }
  if constexpr (vertexUniqueness == VertexUniquenessLevel::GLOBAL) {
    auto const& [unused, added] =
        _uniqueVertices.emplace(step.getVertexIdentifier());
    // If this add fails, we need to exclude this path
    if (!added) {
      return ValidationResult{ValidationResult::Type::FILTER_AND_PRUNE};
    }
    return ValidationResult{ValidationResult::Type::TAKE};
  }

  // For NONE: ignoreOtherValidator return TAKE
  return ValidationResult{ValidationResult::Type::TAKE};
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::exposeUniqueVertices() const
    -> ::arangodb::containers::HashSet<VertexRef, std::hash<VertexRef>,
                                       std::equal_to<VertexRef>> const& {
  return _uniqueVertices;
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    handleValidationResult(ValidationResult validationResult,
                           typename PathStore::Step& step) -> ValidationResult {
  if constexpr (std::is_same_v<ClusterProviderStep, typename PathStore::Step>) {
    step.setValidationResult(validationResult);
  }

  return validationResult;
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    evaluateVertexRestriction(typename PathStore::Step const& step) -> bool {
  if (step.isFirst()) {
    // first step => always allowed
    return true;
  }

  auto const& allowedCollections = _options.getAllowedVertexCollections();
  if (allowedCollections.empty()) {
    // all allowed
    return true;
  }

  auto collectionName = step.getCollectionName();
  if (std::find(allowedCollections.begin(), allowedCollections.end(),
                collectionName) != allowedCollections.end()) {
    // found in allowed collections => allowed
    return true;
  }

  // not allowed
  return false;
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    evaluateVertexCondition(typename PathStore::Step const& step)
        -> ValidationResult {
  // evaluate if vertex collection is allowed
  bool isAllowed = evaluateVertexRestriction(step);
  if (!isAllowed) {
    if (_options.bfsResultHasToIncludeFirstVertex() && step.isFirst()) {
      return ValidationResult{ValidationResult::Type::PRUNE};
    }
    return ValidationResult{ValidationResult::Type::FILTER_AND_PRUNE};
  }

  VPackBuilder vertexBuilder, edgeBuilder;

  // evaluate if vertex needs to be pruned
  ValidationResult res{ValidationResult::Type::TAKE};
  if (_options.usesPrune()) {
    VPackBuilder pathBuilder;

    auto& evaluator = _options.getPruneEvaluator();

    if (evaluator->needsVertex()) {
      _provider.addVertexToBuilder(step.getVertex(), vertexBuilder);
      evaluator->injectVertex(vertexBuilder.slice());
    }
    if (evaluator->needsEdge()) {
      _provider.addEdgeToBuilder(step.getEdge(), edgeBuilder);
      evaluator->injectEdge(edgeBuilder.slice());
    }
    if (evaluator->needsPath()) {
      using ResultPathType =
          SingleProviderPathResult<ProviderType, PathStore, Step>;
      std::unique_ptr<PathResultInterface> currentPath =
          std::make_unique<ResultPathType>(step, _provider, _store);
      currentPath->toVelocyPack(pathBuilder);
      evaluator->injectPath(pathBuilder.slice());
    }
    if (evaluator->evaluate()) {
      res.combine(ValidationResult::Type::PRUNE);
    }
  }

  if (res.isPruned() && res.isFiltered()) {
    return res;
  }

  // Evaluate depth-based vertex expressions
  auto vertexExpr = _options.getVertexExpression(step.getDepth());
  if (vertexExpr != nullptr) {
    if (vertexBuilder.isEmpty()) {
      _provider.addVertexToBuilder(step.getVertex(), vertexBuilder);
    }

    // evaluate expression
    bool satifiesCondition =
        evaluateVertexExpression(vertexExpr, vertexBuilder.slice());
    if (!satifiesCondition) {
      if (_options.bfsResultHasToIncludeFirstVertex() && step.isFirst()) {
        res.combine(ValidationResult::Type::PRUNE);
      } else {
        return ValidationResult{ValidationResult::Type::FILTER_AND_PRUNE};
      }
    }
  }

  // Global edge condition
  auto edgeExpr = _options.getEdgeExpression();
  if (edgeExpr != nullptr) {
    edgeBuilder.clear();
    _provider.addEdgeToBuilder(step.getEdge(), edgeBuilder);

    bool satisfiesCondition =
        evaluateEdgeExpression(edgeExpr, edgeBuilder.slice());
    if (!satisfiesCondition) {
      return ValidationResult{ValidationResult::Type::FILTER_AND_PRUNE};
    }
  }

  if (res.isPruned() && res.isFiltered()) {
    return res;
  }

  if (_options.usesPostFilter()) {
    auto& evaluator = _options.getPostFilterEvaluator();

    if (evaluator->needsVertex()) {
      if (vertexBuilder.isEmpty()) {  // already added a vertex in case
                                      // _options.usesPrune() == true
        _provider.addVertexToBuilder(step.getVertex(), vertexBuilder);
      }
      evaluator->injectVertex(vertexBuilder.slice());
    }
    if (evaluator->needsEdge()) {
      if (edgeBuilder.isEmpty()) {  // already added an edge in case
                                    // _options.usesPrune() == true
        _provider.addEdgeToBuilder(step.getEdge(), edgeBuilder);
      }
      evaluator->injectEdge(edgeBuilder.slice());
    }

    TRI_ASSERT(!evaluator->needsPath());
    if (!evaluator->evaluate()) {
      if (!_options.bfsResultHasToIncludeFirstVertex() || !step.isFirst()) {
        res.combine(ValidationResult::Type::FILTER);
      }
    }
  }
  return res;
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    evaluateEdgeCondition(typename PathStore::Step const& step)
        -> ValidationResult {
  auto res = ValidationResult{ValidationResult::Type::TAKE};

  auto edgeBuilder = VPackBuilder{};

  // Evaluate depth-based vertex expressions
  auto edgeExpr = _options.getEdgeExpression();
  if (edgeExpr != nullptr) {
    if (edgeBuilder.isEmpty()) {
      _provider.addEdgeToBuilder(step.getEdge(), edgeBuilder);
    }

    // evaluate expression
    bool satifiesCondition =
        evaluateEdgeExpression(edgeExpr, edgeBuilder.slice());

    if (!satifiesCondition) {
      if (_options.bfsResultHasToIncludeFirstVertex() && step.isFirst()) {
        res.combine(ValidationResult::Type::PRUNE);
      } else {
        return ValidationResult{ValidationResult::Type::FILTER_AND_PRUNE};
      }
    }
  }

  return res;
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    evaluateVertexExpression(arangodb::aql::Expression* expression,
                             VPackSlice value) -> bool {
  if (expression == nullptr) {
    return true;
  }

  TRI_ASSERT(value.isObject() || value.isNull());
  auto tmpVar = _options.getTempVar();
  bool mustDestroy = false;
  // node: for expression evaluation, the same expression context
  // instance is used repeatedly.
  auto& ctx = _options.getExpressionContext();
  ctx.setVariableValue(tmpVar,
                       aql::AqlValue{aql::AqlValueHintSliceNoCopy{value}});
  // make sure we clean up after ourselves
  ScopeGuard defer([&]() noexcept { ctx.clearVariableValue(tmpVar); });
  aql::AqlValue res = expression->execute(&ctx, mustDestroy);
  aql::AqlValueGuard guard{res, mustDestroy};
  TRI_ASSERT(res.isBoolean());
  return res.toBoolean();
}

// TODO: This is the same code as above, so  this is evaluating an
// expression and it is unclear to me why this function exists
// in this class
template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    evaluateEdgeExpression(arangodb::aql::Expression* expression,
                           VPackSlice value) -> bool {
  if (expression == nullptr) {
    return true;
  }

  TRI_ASSERT(value.isObject() || value.isNull());
  auto tmpVar = _options.getTempVar();
  bool mustDestroy = false;
  // node: for expression evaluation, the same expression context
  // instance is used repeatedly.
  auto& ctx = _options.getExpressionContext();
  ctx.setVariableValue(tmpVar,
                       aql::AqlValue{aql::AqlValueHintSliceNoCopy{value}});
  // make sure we clean up after ourselves
  ScopeGuard defer([&]() noexcept { ctx.clearVariableValue(tmpVar); });
  aql::AqlValue res = expression->execute(&ctx, mustDestroy);
  aql::AqlValueGuard guard{res, mustDestroy};
  TRI_ASSERT(res.isBoolean());
  return res.toBoolean();
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
void PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::reset() {
  if constexpr (vertexUniqueness != VertexUniquenessLevel::NONE) {
    _uniqueVertices.clear();
  }

  if constexpr (edgeUniqueness != EdgeUniquenessLevel::NONE) {
    _uniqueEdges.clear();
  }
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
bool PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::usesPrune() const {
  return _options.usesPrune();
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
bool PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::usesPostFilter() const {
  return _options.usesPostFilter();
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
void PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    setPruneContext(aql::InputAqlItemRow& inputRow) {
  TRI_ASSERT(_options.usesPrune());
  _options.setPruneContext(inputRow);
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
void PathValidator<ProviderType, PathStore, vertexUniqueness, edgeUniqueness>::
    setPostFilterContext(aql::InputAqlItemRow& inputRow) {
  TRI_ASSERT(_options.usesPostFilter());
  _options.setPostFilterContext(inputRow);
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
void PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::unpreparePruneContext() {
  TRI_ASSERT(_options.usesPrune());
  _options.unpreparePruneContext();
}

template<class ProviderType, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
void PathValidator<ProviderType, PathStore, vertexUniqueness,
                   edgeUniqueness>::unpreparePostFilterContext() {
  TRI_ASSERT(_options.usesPostFilter());
  _options.unpreparePostFilterContext();
}

#ifndef USE_ENTERPRISE
template<class Provider, class PathStore,
         VertexUniquenessLevel vertexUniqueness,
         EdgeUniquenessLevel edgeUniqueness>
auto PathValidator<Provider, PathStore, vertexUniqueness, edgeUniqueness>::
    checkValidDisjointPath(typename PathStore::Step const& lastStep)
        -> arangodb::graph::ValidationResult::Type {
  return ValidationResult::Type::TAKE;
}
#endif

namespace arangodb::graph {

/* SingleServerProvider Section */
using SingleServerProviderStep = ::arangodb::graph::SingleServerProviderStep;

template class PathValidator<SingleServerProvider<SingleServerProviderStep>,
                             PathStore<SingleServerProviderStep>,
                             VertexUniquenessLevel::NONE,
                             EdgeUniquenessLevel::NONE>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<SingleServerProviderStep>>,
    PathStoreTracer<PathStore<
        ProviderTracer<SingleServerProvider<SingleServerProviderStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::NONE>;
template class PathValidator<SingleServerProvider<SingleServerProviderStep>,
                             PathStore<SingleServerProviderStep>,
                             VertexUniquenessLevel::NONE,
                             EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<SingleServerProviderStep>>,
    PathStoreTracer<PathStore<
        ProviderTracer<SingleServerProvider<SingleServerProviderStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    SingleServerProvider<SingleServerProviderStep>,
    PathStore<SingleServerProvider<SingleServerProviderStep>::Step>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<SingleServerProviderStep>>,
    PathStoreTracer<PathStore<
        ProviderTracer<SingleServerProvider<SingleServerProviderStep>>::Step>>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    SingleServerProvider<SingleServerProviderStep>,
    PathStore<SingleServerProvider<SingleServerProviderStep>::Step>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<SingleServerProviderStep>>,
    PathStoreTracer<PathStore<
        ProviderTracer<SingleServerProvider<SingleServerProviderStep>>::Step>>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;

#ifdef USE_ENTERPRISE
template class PathValidator<SingleServerProvider<enterprise::SmartGraphStep>,
                             PathStore<enterprise::SmartGraphStep>,
                             VertexUniquenessLevel::NONE,
                             EdgeUniquenessLevel::NONE>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<enterprise::SmartGraphStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        SingleServerProvider<enterprise::SmartGraphStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::NONE>;
template class PathValidator<SingleServerProvider<enterprise::SmartGraphStep>,
                             PathStore<enterprise::SmartGraphStep>,
                             VertexUniquenessLevel::NONE,
                             EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<enterprise::SmartGraphStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        SingleServerProvider<enterprise::SmartGraphStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    SingleServerProvider<enterprise::SmartGraphStep>,
    PathStore<SingleServerProvider<enterprise::SmartGraphStep>::Step>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<enterprise::SmartGraphStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        SingleServerProvider<enterprise::SmartGraphStep>>::Step>>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    SingleServerProvider<enterprise::SmartGraphStep>,
    PathStore<SingleServerProvider<enterprise::SmartGraphStep>::Step>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<SingleServerProvider<enterprise::SmartGraphStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        SingleServerProvider<enterprise::SmartGraphStep>>::Step>>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;
#endif

/* ClusterProvider Section */
template class PathValidator<
    ClusterProvider<ClusterProviderStep>, PathStore<ClusterProviderStep>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::NONE>;
template class PathValidator<
    ProviderTracer<ClusterProvider<ClusterProviderStep>>,
    PathStoreTracer<
        PathStore<ProviderTracer<ClusterProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::NONE>;
template class PathValidator<
    ClusterProvider<ClusterProviderStep>, PathStore<ClusterProviderStep>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<ClusterProvider<ClusterProviderStep>>,
    PathStoreTracer<
        PathStore<ProviderTracer<ClusterProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    ClusterProvider<ClusterProviderStep>, PathStore<ClusterProviderStep>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<ClusterProvider<ClusterProviderStep>>,
    PathStoreTracer<
        PathStore<ProviderTracer<ClusterProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    ClusterProvider<ClusterProviderStep>, PathStore<ClusterProviderStep>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<ClusterProvider<ClusterProviderStep>>,
    PathStoreTracer<
        PathStore<ProviderTracer<ClusterProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;

#ifdef USE_ENTERPRISE
template class PathValidator<
    enterprise::SmartGraphProvider<ClusterProviderStep>,
    PathStore<ClusterProviderStep>, VertexUniquenessLevel::NONE,
    EdgeUniquenessLevel::NONE>;
template class PathValidator<
    ProviderTracer<enterprise::SmartGraphProvider<ClusterProviderStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        enterprise::SmartGraphProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::NONE>;
template class PathValidator<
    enterprise::SmartGraphProvider<ClusterProviderStep>,
    PathStore<ClusterProviderStep>, VertexUniquenessLevel::NONE,
    EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<enterprise::SmartGraphProvider<ClusterProviderStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        enterprise::SmartGraphProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::NONE, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    enterprise::SmartGraphProvider<ClusterProviderStep>,
    PathStore<enterprise::SmartGraphProvider<ClusterProviderStep>::Step>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<enterprise::SmartGraphProvider<ClusterProviderStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        enterprise::SmartGraphProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::PATH, EdgeUniquenessLevel::PATH>;

template class PathValidator<
    enterprise::SmartGraphProvider<ClusterProviderStep>,
    PathStore<enterprise::SmartGraphProvider<ClusterProviderStep>::Step>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;
template class PathValidator<
    ProviderTracer<enterprise::SmartGraphProvider<ClusterProviderStep>>,
    PathStoreTracer<PathStore<ProviderTracer<
        enterprise::SmartGraphProvider<ClusterProviderStep>>::Step>>,
    VertexUniquenessLevel::GLOBAL, EdgeUniquenessLevel::PATH>;
#endif

}  // namespace arangodb::graph
