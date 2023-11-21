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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <type_traits>

#include "Aql/Optimizer/Rules/EnumeratePathsRule.h"
#include "Basics/Common.h"

namespace arangodb::aql {
class ExecutionPlan;
class Optimizer;
struct OptimizerRule;

/// @brief type of an optimizer rule function, the function gets an
/// optimizer, an ExecutionPlan, and the current rule. it has
/// to append one or more plans to the resulting deque. This must
/// include the original plan if it ought to be kept. The rule has to
/// set the level of the appended plan to the largest level of rule
/// that ought to be considered as done to indicate which rule is to be
/// applied next.
typedef void (*RuleFunction)(Optimizer*, std::unique_ptr<ExecutionPlan>,
                             OptimizerRule const&);

/// @brief type of an optimizer rule
struct OptimizerRule {
  enum class Flags : int {
    Default = 0,
    Hidden = 1,
    ClusterOnly = 2,
    CanBeDisabled = 4,
    CanCreateAdditionalPlans = 8,
    DisabledByDefault = 16,
    EnterpriseOnly = 32
  };

  /// @brief helper for building flags
  template<typename... Args>
  static constexpr std::underlying_type<Flags>::type makeFlags(
      Flags flag, Args... args) noexcept {
    return static_cast<std::underlying_type<Flags>::type>(flag) +
           makeFlags(args...);
  }

  static constexpr std::underlying_type<Flags>::type makeFlags() noexcept {
    return static_cast<std::underlying_type<Flags>::type>(Flags::Default);
  }

  /// @brief check a flag for the rule
  constexpr bool hasFlag(Flags flag) const noexcept {
    return ((flags & static_cast<std::underlying_type<Flags>::type>(flag)) !=
            0);
  }

  bool canBeDisabled() const noexcept { return hasFlag(Flags::CanBeDisabled); }

  bool isClusterOnly() const noexcept { return hasFlag(Flags::ClusterOnly); }

  bool isHidden() const noexcept { return hasFlag(Flags::Hidden); }

  bool canCreateAdditionalPlans() const noexcept {
    return hasFlag(Flags::CanCreateAdditionalPlans);
  }

  bool isDisabledByDefault() const noexcept {
    return hasFlag(Flags::DisabledByDefault);
  }

  bool isEnterpriseOnly() const noexcept {
    return hasFlag(Flags::EnterpriseOnly);
  }

  /// @brief optimizer rules
  enum RuleLevel : int {
    // List all the rules in the system here:
    // lower level values mean earlier rule execution

    // note that levels must be unique
    initial = 100,

    // "Pass 1": moving nodes "up" (potentially outside loops):
    // ========================================================
    replaceNearWithinFulltext,

    inlineSubqueriesRule,

    /// simplify some conditions in CalculationNodes
    simplifyConditionsRule,

    // move calculations up the dependency chain (to pull them out of
    // inner loops etc.)
    moveCalculationsUpRule,

    // move filters up the dependency chain (to make result sets as small
    // as possible as early as possible)
    moveFiltersUpRule,

    // remove calculations that are repeatedly used in a query
    removeRedundantCalculationsRule,

    // "Pass 2": try to remove redundant or unnecessary nodes
    // ======================================================

    // remove filters from the query that are not necessary at all
    // filters that are always true will be removed entirely
    // filters that are always false will be replaced with a NoResults node
    removeUnnecessaryFiltersRule,

    // remove calculations that are never necessary
    removeUnnecessaryCalculationsRule,

    // determine the "right" type of CollectNode and
    // add a sort node for each COLLECT (may be removed later)
    specializeCollectRule,

    // remove redundant sort blocks
    removeRedundantSortsRule,

    // push limits into subqueries and simplify them
    optimizeSubqueriesRule,

    // "Pass 3": interchange EnumerateCollection nodes in all possible ways
    //           this is level 500, please never let new plans from higher
    //           levels go back to this or lower levels!
    // ======================================================

    interchangeAdjacentEnumerationsRule,

    // replace attribute accesses that are equal due to a filter statement
    // with the same value. This might enable other optimizations later on.
    replaceEqualAttributeAccesses,

    // "Pass 4": moving nodes "up" (potentially outside loops) (second try):
    // ======================================================

    // move calculations up the dependency chain (to pull them out of
    // inner loops etc.)
    moveCalculationsUpRule2,

    // move filters up the dependency chain (to make result sets as small
    // as possible as early as possible)
    moveFiltersUpRule2,

    /// "Pass 5": try to remove redundant or unnecessary nodes (second try)
    // remove filters from the query that are not necessary at all
    // filters that are always true will be removed entirely
    // filters that are always false will be replaced with a NoResults node
    // ======================================================

    // remove redundant sort blocks
    removeRedundantSortsRule2,

    // remove SORT RAND() if appropriate
    removeSortRandRule,

    // remove INTO for COLLECT if appropriate
    removeCollectVariablesRule,

    // propagate constant attributes in FILTERs
    propagateConstantAttributesRule,

    // remove unused out variables for data-modification queries
    removeDataModificationOutVariablesRule,

    /// "Pass 6": use indexes if possible for FILTER and/or SORT nodes
    // ======================================================

    // replace simple OR conditions with IN
    replaceOrWithInRule,

    // remove redundant OR conditions
    removeRedundantOrRule,

    // remove FILTER and SORT if there are geoindexes
    applyGeoIndexRule,

    useIndexesRule,

    // try to remove filters covered by index ranges
    removeFiltersCoveredByIndexRule,

    removeUnnecessaryFiltersRule2,

    // try to find sort blocks which are superseeded by indexes
    useIndexForSortRule,

    // sort values used in IN comparisons of remaining filters
    sortInValuesRule,

    // Replaces the last element of the path on traversals, by direct output.
    replaceLastAccessOnGraphPathRule,

    // merge filters into graph traversals
    optimizeTraversalsRule,

    // put path filters into enumerate paths
    optimizeEnumeratePathsRule,

    // optimize K_PATHS
    optimizePathsRule,

    // remove redundant filters statements
    removeFiltersCoveredByTraversal,

    // move filters and sort conditions into views and remove them
    handleArangoSearchViewsRule,

    // move constrained sort into views
    handleConstrainedSortInView,

    // remove calculations that are redundant
    // needs to run after filter removal
    removeUnnecessaryCalculationsRule2,

    // remove now obsolete path variables
    removeTraversalPathVariable,

    // when we have single document operations, fill in special cluster
    // handling.
    substituteSingleDocumentOperations,

    // special cluster handling for multiple operations (babies)
    substituteMultipleDocumentOperations,

    /// Pass 9: push down calculations beyond FILTERs and LIMITs
    moveCalculationsDownRule,

    /// Pass 9: fuse filter conditions
    fuseFiltersRule,

  /// "Pass 10": final transformations for the cluster

  // optimize queries in the cluster so that the entire query
  // gets pushed to a single server
  // if applied, this rule will turn all other cluster rules off
  // for the current plan
#ifdef USE_ENTERPRISE
    clusterOneShardRule,
#endif

#ifdef USE_ENTERPRISE
    clusterLiftConstantsForDisjointGraphNodes,
#endif

    // make operations on sharded collections use distribute
    distributeInClusterRule,

#ifdef USE_ENTERPRISE
    smartJoinsRule,
#endif

    // make operations on sharded collections use scatter / gather / remote
    scatterInClusterRule,

#ifdef USE_ENTERPRISE
    // move traversal on SatelliteGraph to db server and add scatter / gather /
    // remote
    scatterSatelliteGraphRule,
#endif

#ifdef USE_ENTERPRISE
    // remove any superfluous SatelliteCollection joins...
    // put it after Scatter rule because we would do
    // the work twice otherwise
    removeSatelliteJoinsRule,

    // remove multiple remote <-> distribute snippets if we are able
    // to combine multiple in only one
    removeDistributeNodesRule,
#endif

#ifdef USE_ENTERPRISE
    // move OffsetInfoMaterialize in between
    // scatter(remote) <-> gather(remote) so they're
    // distributed to the cluster nodes.
    distributeOffsetInfoToClusterRule,
#endif

    // move FilterNodes & Calculation nodes in between
    // scatter(remote) <-> gather(remote) so they're
    // distributed to the cluster nodes.
    distributeFilterCalcToClusterRule,

    // move SortNodes into the distribution.
    // adjust gathernode to also contain the sort criteria.
    distributeSortToClusterRule,

    // moves filters on collection data into EnumerateCollection/Index to
    // avoid copying large amounts of unneeded documents
    moveFiltersIntoEnumerateRule,

    // remove calculations that are redundant
    // this is hidden and disabled by default version
    // used to cleanup calculation nodes after conditionally
    // removed nodes. Currently used by OneShard rule to handle
    // removals of sort nodes for arangosearch views.
    removeUnnecessaryCalculationsRule3,

    // try to get rid of a RemoteNode->ScatterNode combination which has
    // only a SingletonNode and possibly some CalculationNodes as dependencies
    removeUnnecessaryRemoteScatterRule,

    // recognize that a RemoveNode can be moved to the shards
    undistributeRemoveAfterEnumCollRule,

    // push collect operations to the db servers
    collectInClusterRule,

    // make sort node aware of subsequent limit statements for internal
    // optimizations
    applySortLimitRule,

    // simplify an EnumerationCollectionNode that fetches an
    // entire document to a projection of this document
    reduceExtractionToProjectionRule,

    // try to restrict fragments to a single shard if possible
    restrictToSingleShardRule,

    // turns LENGTH(FOR doc IN collection ... RETURN doc) into an optimized
    // count operation
    optimizeCountRule,

    // parallelizes execution in coordinator-sided GatherNodes
    parallelizeGatherRule,

    // reduce a sorted gather to an unsorted gather if only a single shard is
    // affected
    decayUnnecessarySortedGatherRule,

#ifdef USE_ENTERPRISE
    clusterPushSubqueryToDBServer,
#endif

    // move document materialization after SORT and LIMIT
    // this must be run AFTER all cluster rules as this rule
    // needs to take into account query distribution across cluster nodes
    // for arango search view
    lateDocumentMaterializationArangoSearchRule,

    // move document materialization after SORT and LIMIT
    // this must be run AFTER all cluster rules as this rule
    // needs to take into account query distribution across cluster nodes
    // for index
    lateDocumentMaterializationRule,

#ifdef USE_ENTERPRISE
    lateMaterialiationOffsetInfoRule,
#endif

    // replace adjacent index nodes with a join node if the indexes qualify
    // for it.
    joinIndexNodesRule,

    // remove unnecessary projections & store projection attributes in
    // individual registers. must be executed after the joinIndexNodesRule,
    // otherwise the projections handling of JoinNodes will be incorrect.
    optimizeProjectionsRule,

    // final cleanup, after projections
    removeUnnecessaryCalculationsRule4,

    // allows execution nodes to asynchronously prefetch the next batch from
    // their upstream node.
    asyncPrefetchRule,

    // Better to be last, because it doesn't change plan and
    // rely on no one will change search condition after this rule
    immutableSearchConditionRule,

    // splice subquery into the place of a subquery node
    // enclosed by a SubqueryStartNode and a SubqueryEndNode
    // Must run last.
    spliceSubqueriesRule
  };

  static_assert(lateDocumentMaterializationRule < optimizeProjectionsRule);
  static_assert(joinIndexNodesRule < optimizeProjectionsRule);
  static_assert(optimizeProjectionsRule < removeUnnecessaryCalculationsRule4);

#ifdef USE_ENTERPRISE
  static_assert(clusterOneShardRule < distributeInClusterRule);
  static_assert(clusterOneShardRule < smartJoinsRule);
  static_assert(clusterOneShardRule < scatterInClusterRule);

  // SmartJoins must come before we move filters around, so the smart-join
  // detection code does not need to take the special filters into account
  static_assert(smartJoinsRule < moveFiltersIntoEnumerateRule);
#endif

  static_assert(scatterInClusterRule < parallelizeGatherRule);

  static_assert(moveCalculationsUpRule < applySortLimitRule,
                "sort-limit adds/moves limit nodes. And calculations should "
                "not be moved up after that.");
  static_assert(moveCalculationsUpRule2 < applySortLimitRule,
                "sort-limit adds/moves limit nodes. And calculations should "
                "not be moved up after that.");

  static_assert(
      handleConstrainedSortInView < lateDocumentMaterializationArangoSearchRule,
      "Constrained sort optimization outperforms late materialization for "
      "views so it should have a try before late materialization. "
      "Also constrained sort rule now does not expects any late "
      "materialization variables replacement");

  std::string_view name;
  RuleFunction func;
  RuleLevel level;
  std::underlying_type<Flags>::type flags;
  std::string_view description;

  OptimizerRule() = delete;
  OptimizerRule(std::string_view name, RuleFunction const& ruleFunc,
                RuleLevel level, std::underlying_type<Flags>::type flags,
                std::string_view description)
      : name(name),
        func(ruleFunc),
        level(level),
        flags(flags),
        description(description) {}

  OptimizerRule(OptimizerRule&& other) = default;
  OptimizerRule& operator=(OptimizerRule&& other) = default;

  OptimizerRule(OptimizerRule const& other) = delete;
  OptimizerRule& operator=(OptimizerRule const& other) = delete;

  friend bool operator<(OptimizerRule const& lhs, int rhs) {
    return lhs.level < rhs;
  }

  friend bool operator<(int lhs, OptimizerRule const& rhs) {
    return lhs < rhs.level;
  }
};

}  // namespace arangodb::aql
