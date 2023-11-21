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

#include "OptimizerRulesFeature.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/IResearchViewOptimizerRules.h"
#include "Aql/IndexNodeOptimizerRules.h"
#include "Aql/GraphOptimizerRules.h"
#include "Aql/Optimizer/Rules/EnumeratePathsRule.h"
#include "Aql/OptimizerRule.h"
#include "Aql/OptimizerRules.h"
#include "Basics/Exceptions.h"
#include "Cluster/ServerState.h"
#include "FeaturePhases/ClusterFeaturePhase.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"

using namespace arangodb::application_features;

namespace arangodb {
namespace aql {

// @brief list of all rules, sorted by rule level
std::vector<OptimizerRule> OptimizerRulesFeature::_rules;

// @brief lookup from rule name to rule level
std::unordered_map<std::string_view, int> OptimizerRulesFeature::_ruleLookup;

OptimizerRulesFeature::OptimizerRulesFeature(Server& server)
    : ArangodFeature{server, *this} {
  setOptional(false);
#ifdef USE_V8
  startsAfter<V8FeaturePhase>();
#else
  startsAfter<application_features::ClusterFeaturePhase>();
#endif

  startsAfter<AqlFeature>();
}

void OptimizerRulesFeature::collectOptions(
    std::shared_ptr<arangodb::options::ProgramOptions> options) {
  options
      ->addOption("--query.optimizer-rules",
                  "Enable or disable specific optimizer rules by default. "
                  "Specify the rule name prefixed with `-` for disabling, or "
                  "`+` for enabling.",
                  new arangodb::options::VectorParameter<
                      arangodb::options::StringParameter>(&_optimizerRules),
                  arangodb::options::makeDefaultFlags(
                      arangodb::options::Flags::Uncommon))
      .setLongDescription(R"(You can use this option to selectively enable or
disable AQL query optimizer rules by default. You can specify the option
multiple times.

For example, to turn off the rules `use-indexes-for-sort` and
`reduce-extraction-to-projection` by default, use the following:

```
--query.optimizer-rules "-use-indexes-for-sort" --query.optimizer-rules "-reduce-extraction-to-projection"
```

The purpose of this startup option is to be able to enable potential future
experimental optimizer rules, which may be shipped in a disabled-by-default
state.)");

  options
      ->addObsoleteOption(
          "--query.parallelize-gather-writes",
          "Whether to enable write parallelization for gather nodes.", false)
      .setLongDescription(
          "Starting with 3.11 almost all queries support parallelization of "
          "gather nodes, making this option obsolete.");
}

void OptimizerRulesFeature::prepare() {
  addRules();
  enableOrDisableRules();
}

void OptimizerRulesFeature::unprepare() {
  _ruleLookup.clear();
  _rules.clear();
}

OptimizerRule& OptimizerRulesFeature::ruleByLevel(int level) {
  // do a binary search in the sorted rules database
  auto it = std::lower_bound(_rules.begin(), _rules.end(), level);
  if (it != _rules.end() && (*it).level == level) {
    return (*it);
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "unable to find required OptimizerRule");
}

int OptimizerRulesFeature::ruleIndex(int level) {
  auto it = std::lower_bound(_rules.begin(), _rules.end(), level);
  if (it != _rules.end() && (*it).level == level) {
    return static_cast<int>(std::distance(_rules.begin(), it));
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "unable to find required OptimizerRule");
}

OptimizerRule& OptimizerRulesFeature::ruleByIndex(int index) {
  TRI_ASSERT(static_cast<size_t>(index) < _rules.size());
  if (static_cast<size_t>(index) < _rules.size()) {
    return _rules[index];
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "unable to find required OptimizerRule");
}

/// @brief register a rule
void OptimizerRulesFeature::registerRule(
    std::string_view name, RuleFunction func, OptimizerRule::RuleLevel level,
    std::underlying_type<OptimizerRule::Flags>::type flags,
    std::string_view description) {
  // rules must only be added before start()
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  TRI_ASSERT(!_fixed);
#endif

  // duplicate rules are not allowed
  TRI_ASSERT(_ruleLookup.find(name) == _ruleLookup.end());

  LOG_TOPIC("18669", TRACE, Logger::AQL)
      << "adding optimizer rule '" << name << "' with level " << level;

  OptimizerRule rule(name, func, level, flags, description);

  if (rule.isClusterOnly() && !ServerState::instance()->isCoordinator()) {
    // cluster-only rule... however, we are not a coordinator, so we can just
    // ignore this rule
    return;
  }

  _ruleLookup.try_emplace(name, level);
  _rules.push_back(std::move(rule));
}

/// @brief set up the optimizer rules once and forever
void OptimizerRulesFeature::addRules() {
  // List all the rules in the system here:
  // lower level values mean earlier rule execution

  // note that levels must be unique

  registerRule("replace-function-with-index", replaceNearWithinFulltextRule,
               OptimizerRule::replaceNearWithinFulltext,
               OptimizerRule::makeFlags(),
               R"(Replace deprecated index functions such as `FULLTEXT()`,
`NEAR()`, `WITHIN()`, or `WITHIN_RECTANGLE()` with a regular subquery.)");

  // inline subqueries one level higher
  registerRule("inline-subqueries", inlineSubqueriesRule,
               OptimizerRule::inlineSubqueriesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Try to pull subqueries out into their surrounding scope, e.g.
`FOR x IN (FOR y IN collection FILTER y.value >= 5 RETURN y.test) RETURN x.a`
becomes `FOR tmp IN collection FILTER tmp.value >= 5 LET x = tmp.test RETURN x.a`.)");

  // simplify conditions
  registerRule("simplify-conditions", simplifyConditionsRule,
               OptimizerRule::simplifyConditionsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Replace parts in `CalculationNode` expressions with
simpler expressions.)");

  // move calculations up the dependency chain (to pull them out of
  // inner loops etc.)
  registerRule("move-calculations-up", moveCalculationsUpRule,
               OptimizerRule::moveCalculationsUpRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Move calculations up in the processing pipeline as far as
possible (ideally out of enumerations) so they are not executed in loops if not
required. It is quite common that this rule enables further optimizations.)");

  // move filters up the dependency chain (to make result sets as small
  // as possible as early as possible)
  registerRule("move-filters-up", moveFiltersUpRule,
               OptimizerRule::moveFiltersUpRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Move filters up in the processing pipeline as far as possible
(ideally out of inner loops) so they filter results as early as possible.)");

  // remove redundant calculations
  registerRule("remove-redundant-calculations", removeRedundantCalculationsRule,
               OptimizerRule::removeRedundantCalculationsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Replace references to redundant calculations (expressions
with the exact same result) with a single reference, allowing other rules to
remove no longer needed calculations.)");

  // remove filters from the query that are not necessary at all
  // filters that are always true will be removed entirely
  registerRule("remove-unnecessary-filters", removeUnnecessaryFiltersRule,
               OptimizerRule::removeUnnecessaryFiltersRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Remove `FILTER` conditions that always evaluate to `true`.)");

  // remove calculations that are never necessary
  registerRule("remove-unnecessary-calculations",
               removeUnnecessaryCalculationsRule,
               OptimizerRule::removeUnnecessaryCalculationsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Remove all calculations whose result is not referenced in the
query. This can be a consequence of applying other optimizations.)");

  // determine the "right" type of CollectNode and
  // add a sort node for each COLLECT (may be removed later)
  // this rule cannot be turned off (otherwise, the query result might be
  // wrong!)
  registerRule(
      "specialize-collect", specializeCollectRule,
      OptimizerRule::specializeCollectRule,
      OptimizerRule::makeFlags(OptimizerRule::Flags::CanCreateAdditionalPlans,
                               OptimizerRule::Flags::Hidden),
      R"(Appears whenever a `COLLECT` statement is used in a query to determine
the type of `CollectNode` to use.)");

  // remove redundant sort blocks
  registerRule("remove-redundant-sorts", removeRedundantSortsRule,
               OptimizerRule::removeRedundantSortsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Try to merge multiple `SORT` statements into fewer sorts.)");

  // push limits into subqueries and simplify them
  registerRule("optimize-subqueries", optimizeSubqueriesRule,
               OptimizerRule::optimizeSubqueriesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Apply optimizations to subqueries.

This rule adds a `LIMIT` statement to qualifying subqueries to make them return
less data. It also modifies the result value of subqueries in case only the
number of subquery results is checked later. This saves copying the document
data from the subquery to the outer scope and may enable follow-up
optimizations.)");

  /// "Pass 3": interchange EnumerateCollection nodes in all possible ways
  ///           this is level 500, please never let new plans from higher
  ///           levels go back to this or lower levels!
  registerRule(
      "interchange-adjacent-enumerations", interchangeAdjacentEnumerationsRule,
      OptimizerRule::interchangeAdjacentEnumerationsRule,
      OptimizerRule::makeFlags(OptimizerRule::Flags::CanCreateAdditionalPlans,
                               OptimizerRule::Flags::CanBeDisabled),
      R"(Try out permutations of `FOR` statements in queries that contain
multiple loops, which may enable further optimizations by other rules.)");

  // replace attribute accesses that are equal due to a filter statement
  // with the same value. This might enable other optimizations later on.
  registerRule(
      "replace-equal-attribute-accesses", replaceEqualAttributeAccesses,
      OptimizerRule::replaceEqualAttributeAccesses,
      OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
      R"(Replace attribute accesses that are equal due to a filter statement
with the same value. This might enable other optimizations later on.)");

  // "Pass 4": moving nodes "up" (potentially outside loops) (second try):
  // move calculations up the dependency chain (to pull them out of
  // inner loops etc.)
  registerRule("move-calculations-up-2", moveCalculationsUpRule,
               OptimizerRule::moveCalculationsUpRule2,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Second pass of moving calculations up in the processing
pipeline as far as possible, to pull them out of inner loops etc.)");

  // move filters up the dependency chain (to make result sets as small
  // as possible as early as possible)
  registerRule("move-filters-up-2", moveFiltersUpRule,
               OptimizerRule::moveFiltersUpRule2,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Second pass of moving filters up in the processing pipeline
as far as possible so they filter results as early as possible.)");

  // remove redundant sort node
  registerRule("remove-redundant-sorts-2", removeRedundantSortsRule,
               OptimizerRule::removeRedundantSortsRule2,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Second pass of trying to merge multiple `SORT` statements
into fewer sorts.)");

  // remove unused INTO variable from COLLECT, or unused aggregates
  registerRule("remove-collect-variables", removeCollectVariablesRule,
               OptimizerRule::removeCollectVariablesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Remove `INTO` and `AGGREGATE` clauses from `COLLECT`
statements if the result is not used.)");

  // propagate constant attributes in FILTERs
  registerRule("propagate-constant-attributes", propagateConstantAttributesRule,
               OptimizerRule::propagateConstantAttributesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Insert constant values into `FILTER` conditions, replacing
dynamic attribute values.)");

  // remove unused out variables for data-modification queries
  registerRule("remove-data-modification-out-variables",
               removeDataModificationOutVariablesRule,
               OptimizerRule::removeDataModificationOutVariablesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Avoid setting the pseudo-variables `OLD` and `NEW` if they
are not used in data modification queries.)");

  /// "Pass 6": use indexes if possible for FILTER and/or SORT nodes
  // try to replace simple OR conditions with IN
  registerRule("replace-or-with-in", replaceOrWithInRule,
               OptimizerRule::replaceOrWithInRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Combine multiple `OR` equality conditions on the same
variable or attribute with an `IN` condition.)");

  // try to remove redundant OR conditions
  registerRule("remove-redundant-or", removeRedundantOrRule,
               OptimizerRule::removeRedundantOrRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Combine multiple `OR` conditions for the same variable or
attribute into a single condition.)");

  // remove FILTER DISTANCE(...) and SORT DISTANCE(...)
  registerRule("geo-index-optimizer", geoIndexRule,
               OptimizerRule::applyGeoIndexRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Utilize geo-spatial indexes.)");

  // try to find a filter after an enumerate collection and find indexes
  registerRule("use-indexes", useIndexesRule, OptimizerRule::useIndexesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Use indexes to iterate over collections, replacing
`EnumerateCollectionNode` with `IndexNode` in the query plan.)");

  // try to remove filters which are covered by index ranges
  registerRule("remove-filter-covered-by-index",
               removeFiltersCoveredByIndexRule,
               OptimizerRule::removeFiltersCoveredByIndexRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Replace or remove `FilterNode` if the filter conditions are
already covered by `IndexNode`.)");

  /// "Pass 5": try to remove redundant or unnecessary nodes (second try)
  // remove filters from the query that are not necessary at all
  // filters that are always true will be removed entirely
  registerRule("remove-unnecessary-filters-2", removeUnnecessaryFiltersRule,
               OptimizerRule::removeUnnecessaryFiltersRule2,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Second pass of removing `FILTER` conditions that always
evaluate to `true`.)");

  // try to find sort blocks which are superseded by indexes
  registerRule("use-index-for-sort", useIndexForSortRule,
               OptimizerRule::useIndexForSortRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Use indexes to avoid `SORT` operations, removing `SortNode`
from the query plan.)");

  // sort in-values in filters (note: must come after
  // remove-filter-covered-by-index rule)
  registerRule("sort-in-values", sortInValuesRule,
               OptimizerRule::sortInValuesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Use a binary search for in-list lookups with a logarithmic
complexity instead of the default linear complexity in-list lookup if the
comparison array on the right-hand side of an `IN` operator is pre-sorted by an
extra function call.)");

  // Replaces the last element of the path on traversals, by direct output.
  // path.vertices[-1] => v and path.edges[-1] => e
  registerRule("optimize-traversal-last-element-access",
               replaceLastAccessOnGraphPathRule,
               OptimizerRule::replaceLastAccessOnGraphPathRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Transform accesses to the last vertex or edge of the path
output variable (`p.vertices[-1]` and `p.edges[-1]`) emitted by AQL traversals
(`FOR v, e, p IN ...`) with accesses to the vertex or edge variable
(`v` and `e`). This can avoid computing the path variable at all and enable
further optimizations that are not possible on the path variable `p`.)");

  // merge filters into traversals
  registerRule("optimize-traversals", optimizeTraversalsRule,
               OptimizerRule::optimizeTraversalsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Try to move `FILTER` conditions into `TraversalNode` for
early pruning of results, apply traversal projections, and avoid calculating
edge and path output variables that are not declared in the query for the
AQL traversal.)");

  // merge certain filters into ENUMERATE_PATHS
  registerRule("optimize-enumerate-paths", optimizeEnumeratePathsRule,
               OptimizerRule::optimizeEnumeratePathsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Try to move `FILTER` conditions into `EnumeratePathsNode` for
pre-filtering the graph that is searched.)");

  // optimize K_PATHS
  registerRule(
      "optimize-paths", optimizePathsRule, OptimizerRule::optimizePathsRule,
      OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
      R"(Check how the output variables of `K_PATHS`, `K_SHORTEST_PATHS`,
and `ALL_SHORTEST_PATHS` path search graph algorithms are used and avoid
loading the vertex documents if they are not accessed in the query.)");

  // optimize unnecessary filters already applied by the traversal
  registerRule("remove-filter-covered-by-traversal",
               removeFiltersCoveredByTraversal,
               OptimizerRule::removeFiltersCoveredByTraversal,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Replace or remove `FilterNode` if the filter conditions are
already covered by `TraversalNode`.)");

  // move search and scorers into views
  registerRule(
      "handle-arangosearch-views", arangodb::iresearch::handleViewsRule,
      OptimizerRule::handleArangoSearchViewsRule, OptimizerRule::makeFlags(),
      R"(Appears whenever an `arangosearch` or `search-alias` View is accessed
in a query.)");

  // move constrained sort into views
  registerRule("arangosearch-constrained-sort",
               arangodb::iresearch::handleConstrainedSortInView,
               OptimizerRule::handleConstrainedSortInView,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Make nodes of type `EnumerateViewNode` aware of `SORT` with a
subsequent `LIMIT` when using Views to reduce memory usage and avoid unnecessary
sorting that has already been carried out by ArangoSearch internally.)");

  // remove calculations that are never necessary
  registerRule("remove-unnecessary-calculations-2",
               removeUnnecessaryCalculationsRule,
               OptimizerRule::removeUnnecessaryCalculationsRule2,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Second pass of removing all calculations whose result is not
referenced in the query. This can be a consequence of applying other
optimizations)");

  // optimize unnecessary filters already applied by the traversal. Only ever
  // does something if previous rules remove all filters using the path variable
  registerRule("remove-redundant-path-var", removeTraversalPathVariable,
               OptimizerRule::removeTraversalPathVariable,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Avoid computing the variables emitted by AQL traversals if
they are declared but unused in the query, or only used in filters that are
pulled into the traversal, significantly reducing overhead.)");

  registerRule(
      "optimize-projections", optimizeProjections,
      OptimizerRule::optimizeProjectionsRule,
      OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
      R"(Remove projections that are no longer used and store projection
results in separate output registers.)");

  registerRule("optimize-cluster-single-document-operations",
               substituteClusterSingleDocumentOperationsRule,
               OptimizerRule::substituteSingleDocumentOperations,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Let a Coordinator work with a document directly if you
reference a document by its `_key`. In this case, no AQL is executed on the
DB-Servers.)");

  registerRule("optimize-cluster-multiple-document-operations",
               substituteClusterMultipleDocumentOperationsRule,
               OptimizerRule::substituteMultipleDocumentOperations,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(For bulk `INSERT` operations in cluster deployments, avoid
unnecessary overhead that AQL queries typically require for the setup and
shutdown in clusters, as well as for the internal batching.

This optimization also decreases the number of HTTP requests to the DB-Servers.

The following patterns are recognized:

- `FOR doc IN @docs INSERT doc INTO collection`, where `@docs` is a
  bind parameter with an array of documents to be inserted
- `FOR doc IN [ { … }, { … }, … ] INSERT doc INTO collection`, where the `FOR`
  loop iterates over an array of input documents known at query compile time
- `LET docs = [ { … }, { … }, … ] FOR doc IN docs INSERT doc INTO collection`,
  where the `docs` variable is a static array of input documents known at
  query compile time

If a query has such a pattern, and all of the following restrictions are met,
then the optimization is triggered:

- There are no following `RETURN` nodes (including any `RETURN OLD` or `RETURN NEW`)
- The `FOR` loop is not contained in another outer `FOR` loop or subquery
- There are no other operations (e.g. `LET`, `FILTER`) between `FOR` and `INSERT`
- `INSERT` is not used on a SmartGraph edge collection
- The `FOR` loop iterates over a constant, deterministic expression

The optimization then replaces the `InsertNode` and `EnumerateListNode` with a
`MultipleRemoteExecutionNode` in the query execution plan, which takes care of
inserting all documents into the collection in one go. Further optimizer rules
are skipped if the optimization is triggered.
)");

  // make sort node aware of subsequent limit statements for internal
  // optimizations
  registerRule("sort-limit", sortLimitRule, OptimizerRule::applySortLimitRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Make `SORT` aware of a subsequent `LIMIT` to enable
optimizations internal to the `SortNode` that allow to reduce memory usage
and, in many cases, improve the sorting speed.

A `SortNode` needs to be followed by a `LimitNode` with no intervening nodes
that may change the element count (e.g. a `FilterNode` which cannot be moved
before the sort, or a source node like `EnumerateCollectionNode`).

The optimizer may choose not to apply the rule if it decides that it offers
little or no benefit. In particular, it does not apply the rule if the input
size is very small or if the output from the `LimitNode` is similar in size to
the input. In exceptionally rare cases, this rule could result in some small
slowdown. If observed, you can disable the rule for the affected query at the
cost of increased memory usage.)");

  // finally, push calculations as far down as possible
  registerRule("move-calculations-down", moveCalculationsDownRule,
               OptimizerRule::moveCalculationsDownRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Move calculations down in the processing pipeline as far as
possible (below `FILTER`, `LIMIT` and `SUBQUERY` nodes) so they are executed as
late as possible and not before their results are required.)");

  // fuse multiple adjacent filters into one
  registerRule("fuse-filters", fuseFiltersRule, OptimizerRule::fuseFiltersRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Merges adjacent `FILTER` nodes together into a single
`FILTER` node.)");

#ifdef USE_ENTERPRISE
  // must be the first cluster optimizer rule
  registerRule("cluster-one-shard", clusterOneShardRule,
               OptimizerRule::clusterOneShardRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Offload the entire query to the DB-Server (except the client
communication via a Coordinator). This saves all the back and forth that
normally exists in regular cluster queries, benefitting traversals and joins
in particular.

Only for eligible queries in the OneShard deployment mode as well as for
queries that only involve collection(s) with a single shard (and identical
sharding in case of multiple collections, e.g. via `distributeShardsLike`).
Queries involving V8 / JavaScript (e.g. user-defined AQL functions) or
SmartGraphs cannot be optimized.)");
#endif

#ifdef USE_ENTERPRISE
  // must run before distribute-in-cluster and must not be disabled, as it is
  // necessary for distributing SmartGraph operations
  registerRule("cluster-lift-constant-for-disjoint-graph-nodes",
               clusterLiftConstantsForDisjointGraphNodes,
               OptimizerRule::clusterLiftConstantsForDisjointGraphNodes,
               OptimizerRule::makeFlags(OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Detect SmartGraph traversals with a constant start vertex to
prepare follow-up optimizations that can determine the shard location and push
down calculations to a DB-Server.)");
#endif

  registerRule("distribute-in-cluster", distributeInClusterRule,
               OptimizerRule::distributeInClusterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::ClusterOnly),
               R"(Appears if query parts get distributed in a cluster.)");

#ifdef USE_ENTERPRISE
  registerRule("smart-joins", smartJoinsRule, OptimizerRule::smartJoinsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Reduce inter-node joins to server-local joins.
This rule is only employed when joining two collections with identical sharding
setup via their shard keys.)");
#endif

  // distribute operations in cluster
  registerRule("scatter-in-cluster", scatterInClusterRule,
               OptimizerRule::scatterInClusterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::ClusterOnly),
               R"(Appears if nodes of the types `ScatterNode`, `GatherNode`,
and `RemoteNode` are inserted into a distributed query plan.)");

#ifdef USE_ENTERPRISE
  registerRule("distribute-offset-info-to-cluster",
               distributeOffsetInfoToClusterRule,
               OptimizerRule::distributeOffsetInfoToClusterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Push the calculation of search highlighting information to
DB-Servers where the data for determining the offsets is stored.)");
#endif

  registerRule("distribute-filtercalc-to-cluster",
               distributeFilterCalcToClusterRule,
               OptimizerRule::distributeFilterCalcToClusterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Move filters up in a distributed execution plan. Filters are
moved as far up in the plan as possible to make result sets as small as
possible, as early as possible.)");

  registerRule("distribute-sort-to-cluster", distributeSortToClusterRule,
               OptimizerRule::distributeSortToClusterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Move sort operations up in a distributed query. Sorts are
moved as far up in the query plan as possible to make result sets as small as
possible, as early as possible.)");

  // remove calculations that are never necessary
  registerRule("remove-unnecessary-calculations-3",
               removeUnnecessaryCalculationsRule,
               OptimizerRule::removeUnnecessaryCalculationsRule3,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::DisabledByDefault,
                                        OptimizerRule::Flags::Hidden),
               R"(Third pass of removing all calculations whose result is not
referenced in the query. This can be a consequence of applying other
optimizations)");

  registerRule("remove-unnecessary-remote-scatter",
               removeUnnecessaryRemoteScatterRule,
               OptimizerRule::removeUnnecessaryRemoteScatterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Avoid distributing calculations and handle them centrally if
a `RemoteNode` is followed by a `ScatterNode`, and the `ScatterNode` is only
followed by calculations or a `SingletonNode`.)");

#ifdef USE_ENTERPRISE
  registerRule("scatter-satellite-graphs", scatterSatelliteGraphRule,
               OptimizerRule::scatterSatelliteGraphRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Execute nodes of the types `TraversalNode`,
`ShortestPathNode`, and `KShortestPathsNode` on a DB-Server instead of on a
Coordinator if the nodes operate on SatelliteGraphs, removing the need to
transfer data for these nodes.)");

  registerRule("remove-satellite-joins", removeSatelliteJoinsRule,
               OptimizerRule::removeSatelliteJoinsRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Optimize nodes of the types `ScatterNode`, `GatherNode`, and
`RemoteNode` for SatelliteCollections and SatelliteGraphs away. Execute the
respective query parts on each participating DB-Server independently, so that
the results become available locally without network communication.
Depends on the `remove-unnecessary-remote-scatter` rule.)");

  registerRule("remove-distribute-nodes", removeDistributeNodesRule,
               OptimizerRule::removeDistributeNodesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Combine multiples nodes of type `DistributeNode` into one if
two adjacent `DistributeNode` nodes share the same input variables and
therefore can be optimized into a single `DistributeNode`.)");
#endif

  registerRule("undistribute-remove-after-enum-coll",
               undistributeRemoveAfterEnumCollRule,
               OptimizerRule::undistributeRemoveAfterEnumCollRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Push nodes of type `RemoveNode` into the same query part that
enumerates over the documents of a collection. This saves inter-cluster
roundtrips between the `EnumerateCollectionNode` and the `RemoveNode`.
It includes simple `UPDATE` and `REPLACE` operations that modify multiple
documents and do not use `LIMIT`.)");

  registerRule("collect-in-cluster", collectInClusterRule,
               OptimizerRule::collectInClusterRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Perform the heavy processing for `COLLECT` statements on
DB-Servers and only light-weight aggregation on a Coordinator. Both sides get
a `CollectNode` in the query plan.)");

  registerRule("restrict-to-single-shard", restrictToSingleShardRule,
               OptimizerRule::restrictToSingleShardRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Restrict operations to a single shard instead of applying
them for all shards if a collection operation (`IndexNode` or a
data modification node) only affects a single shard.
  
This optimization can be applied for queries that access a collection only once
in the query, and that do not use traversals, shortest path queries, and that
do not access collection data dynamically using the `DOCUMENT()`, `FULLTEXT()`,
`NEAR()` or `WITHIN()` AQL functions. Additionally, the optimizer can only
apply this optimization if it can safely determine the values of all the
collection's shard keys from the query, and when the shard keys are covered by
a single index (this is always true if the shard key is the default `_key`).)");

  registerRule("move-filters-into-enumerate", moveFiltersIntoEnumerateRule,
               OptimizerRule::moveFiltersIntoEnumerateRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Move filters on non-indexed collection attributes into
`IndexNode` or `EnumerateCollectionNode` to allow early pruning of
non-matching documents. This optimization can help to avoid a lot of temporary
document copies.)");

  registerRule("optimize-count", optimizeCountRule,
               OptimizerRule::optimizeCountRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Optimize subqueries to use an optimized code path for
counting documents.

The requirements are that the subquery result must only be used with the
`COUNT()` or `LENGTH()` AQL function and not for anything else. The subquery
itself must be read-only (no data modification subquery), not use nested `FOR`
loops, no `LIMIT` statement, and no `FILTER` condition or calculation that
requires accessing document data. Accessing index data is supported for
filtering but not for further calculations.)");

  registerRule("parallelize-gather", parallelizeGatherRule,
               OptimizerRule::parallelizeGatherRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Apply an optimization to execute Coordinator `GatherNode`
nodes in parallel. These notes cannot be parallelized if they depend on a
`TraversalNode`, except for certain Disjoint SmartGraph traversals where the
traversal can run completely on the local DB-Server.)");

  registerRule("decay-unnecessary-sorted-gather", decayUnnecessarySortedGather,
               OptimizerRule::decayUnnecessarySortedGatherRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly),
               R"(Avoid merge-sorting results on a Coordinator if they are all
from a single shard and fully sorted by a DB-Server already.)");

#ifdef USE_ENTERPRISE
  registerRule("push-subqueries-to-dbserver", clusterPushSubqueryToDBServer,
               OptimizerRule::clusterPushSubqueryToDBServer,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::ClusterOnly,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Execute subqueries entirely on a DB-Server if possible.
Subqueries need to contain exactly one distribute/gather section, and only one
collection access or traversal, shortest path, or k-shortest paths query.)");
#endif

  // apply late materialization for view queries
  registerRule("late-document-materialization-arangosearch",
               iresearch::lateDocumentMaterializationArangoSearchRule,
               OptimizerRule::lateDocumentMaterializationArangoSearchRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Try to read from the underlying collections of a View as late
as possible if the involved attributes are covered by the View index.)");

  // apply late materialization for index queries
  registerRule("late-document-materialization", lateDocumentMaterializationRule,
               OptimizerRule::lateDocumentMaterializationRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Try to read from collections as late as possible if the
involved attributes are covered by regular indexes.)");

#ifdef USE_ENTERPRISE
  // apply late materialization for offset infos
  registerRule("late-materialization-offset-info",
               lateMaterialiationOffsetInfoRule,
               OptimizerRule::lateMaterialiationOffsetInfoRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled,
                                        OptimizerRule::Flags::EnterpriseOnly),
               R"(Get the search highlighting offsets as late as possible to
avoid unnecessary reads.)");
#endif

  registerRule(
      "immutable-search-condition", iresearch::immutableSearchCondition,
      OptimizerRule::immutableSearchConditionRule,
      OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
      R"(Optimize immutable search condition for nested loops, we don't need to make real search many times, if we can cache results in bitset)");

  // remove calculations that are never necessary
  registerRule("remove-unnecessary-calculations-4",
               removeUnnecessaryCalculationsRule,
               OptimizerRule::removeUnnecessaryCalculationsRule4,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Fourth pass of removing all calculations whose result is not
referenced in the query. This can be a consequence of applying other
optimizations)");

  // add the storage-engine specific rules
  addStorageEngineRules();

  // Splice subqueries
  //
  // ***CAUTION***
  // TL;DR: This rule (if activated) *must* run last.
  //
  // It changes the structure of the query plan by "splicing", i.e. replacing
  // every SubqueryNode by a SubqueryStart and a SubqueryEnd node with the
  // subquery's nodes in between, resulting in a linear query plan. If an
  // optimizer rule runs after this rule, it has to be aware of
  // SubqueryStartNode and SubqueryEndNode and would likely be more complicated
  // to write. note: since 3.8 this rule cannot be turned off anymore. this also
  // means that any query execution plan created by 3.8 will not contain nodes
  // of type SUBQUERY after optimization. it is still possible that 3.8
  // encounters plan with SUBQUERY node types inside, e.g. if they come from 3.7
  // coordinators during rolling upgrades.
  registerRule("splice-subqueries", spliceSubqueriesRule,
               OptimizerRule::spliceSubqueriesRule, OptimizerRule::makeFlags(),
               R"(Appears if subqueries are spliced into the surrounding query,
reducing overhead for executing subqueries by inlining the execution.
This mainly benefits queries which execute subqueries very often that only
return a few results at a time.

This optimization is performed on all subqueries and is applied after all other
optimizations.)");

  // replace adjacent index nodes with a join node if the indexes qualify
  // for it.
  registerRule("join-index-nodes", joinIndexNodesRule,
               OptimizerRule::joinIndexNodesRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Join adjacent index nodes and replace them with a join node
in case the indexes qualify for it.)");

  // allow nodes to asynchronously prefetch the next batch while processing the
  // current batch. this effectively allows parts of the query to run in
  // parallel. this is only supported by certain types of nodes and queries.
  registerRule("async-prefetch", asyncPrefetchRule,
               OptimizerRule::asyncPrefetchRule,
               OptimizerRule::makeFlags(OptimizerRule::Flags::CanBeDisabled),
               R"(Allow query execution nodes to asynchronously prefetch the
next batch while processing the current batch, allowing parts of the query to
run in parallel. This is only possible for certain operations in a query.)");

  // finally sort all rules by their level
  std::sort(_rules.begin(), _rules.end(),
            [](OptimizerRule const& lhs, OptimizerRule const& rhs) noexcept {
              return (lhs.level < rhs.level);
            });

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // make the rules database read-only from now on
  _fixed = true;
#endif
}

void OptimizerRulesFeature::addStorageEngineRules() {
  StorageEngine& engine = server().getFeature<EngineSelectorFeature>().engine();
  engine.addOptimizerRules(*this);
}

/// @brief translate a list of rule ids into rule names
std::vector<std::string_view> OptimizerRulesFeature::translateRules(
    std::vector<int> const& rules) {
  std::vector<std::string_view> names;
  names.reserve(rules.size());

  for (auto const& rule : rules) {
    std::string_view name = translateRule(rule);

    if (!name.empty()) {
      names.emplace_back(name);
    }
  }
  return names;
}

/// @brief translate a single rule
std::string_view OptimizerRulesFeature::translateRule(int level) {
  // do a binary search in the sorted rules database
  auto it = std::lower_bound(_rules.begin(), _rules.end(), level);
  if (it != _rules.end() && (*it).level == level) {
    return (*it).name;
  }

  return std::string_view();
}

/// @brief translate a single rule
int OptimizerRulesFeature::translateRule(std::string_view name) {
  auto it = _ruleLookup.find(name);

  if (it != _ruleLookup.end()) {
    return (*it).second;
  }

  return -1;
}

void OptimizerRulesFeature::enableOrDisableRules() {
  // turn off or on specific optimizer rules, based on startup parameters
  for (auto const& name : _optimizerRules) {
    std::string_view n(name);
    if (!n.empty() && n.front() == '+') {
      // strip initial + sign
      n = n.substr(1);
    }

    if (n.empty()) {
      continue;
    }

    if (n.front() == '-') {
      n = n.substr(1);
      // disable
      if (n == "all") {
        for (auto& rule : _rules) {
          if (rule.hasFlag(OptimizerRule::Flags::CanBeDisabled)) {
            rule.flags |= OptimizerRule::makeFlags(
                OptimizerRule::Flags::DisabledByDefault);
          }
        }
      } else {
        int id = translateRule(n);
        if (id != -1) {
          auto& rule = ruleByLevel(id);
          if (rule.hasFlag(OptimizerRule::Flags::CanBeDisabled)) {
            rule.flags |= OptimizerRule::makeFlags(
                OptimizerRule::Flags::DisabledByDefault);
          }
        } else {
          LOG_TOPIC("6103b", WARN, Logger::QUERIES)
              << "cannot disable unknown optimizer rule '" << n << "'";
        }
      }
    } else {
      if (n == "all") {
        for (auto& rule : _rules) {
          rule.flags &= ~OptimizerRule::makeFlags(
              OptimizerRule::Flags::DisabledByDefault);
        }
      } else {
        int id = translateRule(n);
        if (id != -1) {
          auto& rule = ruleByLevel(id);
          rule.flags &= ~OptimizerRule::makeFlags(
              OptimizerRule::Flags::DisabledByDefault);
        } else {
          LOG_TOPIC("aa52f", WARN, Logger::QUERIES)
              << "cannot enable unknown optimizer rule '" << n << "'";
        }
      }
    }
  }
}

}  // namespace aql
}  // namespace arangodb
