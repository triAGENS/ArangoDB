////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2025 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/Condition.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode/CalculationNode.h"
#include "Aql/ExecutionNode/CollectNode.h"
#include "Aql/ExecutionNode/ExecutionNode.h"
#include "Aql/ExecutionNode/IndexCollectNode.h"
#include "Aql/ExecutionNode/IndexNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/IndexDistrinctScan.h"
#include "Aql/IndexStreamIterator.h"
#include "Aql/Optimizer.h"
#include "Aql/OptimizerRules.h"
#include "Aql/Query.h"
#include "Indexes/Index.h"
#include "Logger/LogMacros.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::containers;
using EN = arangodb::aql::ExecutionNode;

#define LOG_RULE LOG_DEVEL_IF(false)

namespace {

bool isCollectNodeEligible(CollectNode const& cn) {
  if (cn.hasOutVariable()) {
    // TODO instead of using the out variable one could translate this
    //  into a PUSH aggregation. This only works if an expression is used
    //  and no KEEP statement is present.
    LOG_RULE << "Collect " << cn.id()
             << " not eligible - it has an out variable";
    return false;
  }

  for (auto const& agg : cn.aggregateVariables()) {
    // TODO this happens with COUNT INTO. Maybe support this later on?
    if (agg.inVar == nullptr) {
      LOG_RULE << "Collect " << cn.id()
               << " not eligible - aggregation of type " << agg.type
               << " into variable " << agg.outVar->name
               << " has no input variable.";
      return false;
    }
  }

  if (cn.getOptions().disableIndex) {
    LOG_RULE << "Collect " << cn.id()
             << " not eligible - index usage explicitly disabled";
    return false;
  }

  return true;
}

bool isIndexNodeEligible(IndexNode const& in) {
  if (in.hasFilter()) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - it has a post filter";
    return false;
  }
  if (in.isLateMaterialized()) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - it is in late materialize mode";
    return false;
  }
  if (in.getIndexes().size() != 1) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - it uses multiple indexes";
    return false;
  }
  auto index = in.getSingleIndex();
  if (index->type() != arangodb::Index::TRI_IDX_TYPE_PERSISTENT_INDEX) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - only persistent index supported";
    return false;
  }
  if (not in.projections().usesCoveringIndex()) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - index does not cover everything";
    return false;
  }
  if (not in.condition()->isEmpty()) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - index with condition is not yet supported";
    return false;
  }
  if (index->sparse()) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - sparse indexes not yet supported";
    return false;
  }
  // TODO get rid
  if (index->unique()) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - optimization not feasible for unique indexes";
    return false;
  }

  if (not in.options().ascending) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - descending mode not yet supported";
    return false;
  }

  return true;
}

bool checkSelectivityFitsAlgorithm(IndexNode const& in) {
  auto index = in.getSingleIndex();
  // assume there are n documents in the collection and we have
  // k distinct features. A linear search is in O(n) while a distinct scan
  // requires O(k log n). So checking for k log n < n, it follows
  // k / n < 1 / log n, where k / n is precisely the selectivity estimate.
  double selectivity = index->selectivityEstimate();
  auto numberOfItems = in.estimateCost().estimatedNrItems;

  double const requiredSelectivity = 1. / log(numberOfItems);

  if (selectivity > requiredSelectivity) {
    LOG_RULE << "IndexNode " << in.id()
             << " not eligible - selectivity is too high, actual = "
             << selectivity << " max allowed = " << requiredSelectivity;
    return false;
  }

  return true;
}

bool attributeMatches(aql::AttributeNamePath const& path,
                      std::vector<basics::AttributeName> const& field) {
  if (path.size() != field.size()) {
    return false;
  }

  for (size_t k = 0; k < path.size(); k++) {
    if (field[k].shouldExpand) {
      return false;
    }

    if (path[k] != field[k].name) {
      return false;
    }
  }
  return true;
}

std::optional<std::tuple<std::vector<CalculationNode*>, IndexCollectGroups,
                         IndexCollectAggregations>>
isEligiblePair(ExecutionPlan const& plan, CollectNode const& cn,
               IndexNode const& idx) {
  IndexDistinctScanOptions scanOptions;
  std::vector<CalculationNode*> calculationsToRemove;
  IndexCollectGroups groups;
  IndexCollectAggregations aggregations;
  std::vector<std::size_t> aggregationFields;

  auto const& coveredFields = idx.getSingleIndex()->coveredFields();
  auto const& keyFields = idx.getSingleIndex()->fields();

  // every group variable has to be a field of the index
  for (auto const& grp : cn.groupVariables()) {
    // get the producing node for the in variable, it should be a calculation
    auto producer = plan.getVarSetBy(grp.inVar->id);
    if (producer->getType() != EN::CALCULATION) {
      return std::nullopt;
    }

    // check if it is just an attribute access for the out variable of the
    // index node.
    auto calculation = EN::castTo<CalculationNode*>(producer);
    std::pair<Variable const*, std::vector<arangodb::basics::AttributeName>>
        access;
    bool const isAttributeAccess =
        calculation->expression()->node()->isAttributeAccessForVariable(access);
    if (not isAttributeAccess || access.first->id != idx.outVariable()->id) {
      LOG_RULE
          << "Calculation " << calculation->id()
          << " not eligible - expression is not an attribute access on index ("
          << idx.id() << ") out variable";
      return std::nullopt;
    }

    // check if this attribute is a covered field
    auto iter = std::find(keyFields.begin(), keyFields.end(), access.second);
    if (iter == keyFields.end()) {
      LOG_RULE << "Calculation " << calculation->id()
               << " not eligible - accessed attribute " << access.second
               << " which is not covered by the index";
      return std::nullopt;
    }

    std::size_t fieldIndex = std::distance(keyFields.begin(), iter);
    scanOptions.distinctFields.push_back(fieldIndex);
    calculationsToRemove.push_back(calculation);
    groups.emplace_back(
        IndexCollectGroup{.indexField = fieldIndex, .outVariable = grp.outVar});
  }

  for (auto const& agg : cn.aggregateVariables()) {
    // get the producing node for the in variable, it should be a calculation
    auto producer = plan.getVarSetBy(agg.inVar->id);
    if (producer->getType() != EN::CALCULATION) {
      return std::nullopt;
    }

    // check if it is just an attribute access for the out variable of the
    // index node.
    auto calculation = EN::castTo<CalculationNode*>(producer);

    VarSet variablesUsed;
    calculation->expression()->variables(variablesUsed);
    if (variablesUsed != VarSet{idx.outVariable()}) {
      LOG_RULE << "Calculation " << calculation->id()
               << " not eligible - expression uses other variables than index ("
               << idx.id() << ") out variable " << idx.outVariable()->name;
      return std::nullopt;
    }

    containers::FlatHashSet<aql::AttributeNamePath> attributes;
    Ast::getReferencedAttributesRecursive(
        calculation->expression()->node(), idx.outVariable(), "", attributes,
        plan.getAst()->query().resourceMonitor());

    // check if all attributes are covered fields
    for (auto const& attr : attributes) {
      auto iter = std::find_if(
          coveredFields.begin(), coveredFields.end(),
          [&](auto const& field) { return attributeMatches(attr, field); });
      if (iter == coveredFields.end()) {
        LOG_RULE << "Calculation " << calculation->id()
                 << " not eligible - accessed attribute " << attr
                 << " which is not covered by the index";
        return std::nullopt;
      }
      std::size_t fieldIndex = std::distance(coveredFields.begin(), iter);
      aggregationFields.emplace_back(fieldIndex);
    }

    aggregations.emplace_back(IndexCollectAggregation{
        .type = agg.type,
        .outVariable = agg.outVar,
        .expression = calculation->expression()->clone(plan.getAst())});
  }

  scanOptions.sorted = cn.getOptions().requiresSortedInput();

  if (aggregations.empty()) {
    if (not idx.getSingleIndex()->supportsDistinctScan(scanOptions)) {
      LOG_RULE
          << "Index " << idx.id()
          << " not eligible - index does not support required distinct scan "
          << scanOptions;
      return std::nullopt;
    }
  } else {
    IndexStreamOptions streamOptions;
    streamOptions.usedKeyFields = scanOptions.distinctFields;
    streamOptions.projectedFields = aggregationFields;
    if (auto support =
            idx.getSingleIndex()->supportsStreamInterface(streamOptions);
        !support.hasSupport()) {
      LOG_RULE
          << "Index " << idx.id()
          << " not eligible - index does not support required stream operation "
          << streamOptions;
      return std::nullopt;
    }
  }

  return std::make_tuple(std::move(calculationsToRemove), std::move(groups),
                         std::move(aggregations));
}

}  // namespace

void arangodb::aql::useIndexForCollect(Optimizer* opt,
                                       std::unique_ptr<ExecutionPlan> plan,
                                       OptimizerRule const& rule) {
  bool modified = false;

  containers::SmallVector<ExecutionNode*, 8> collectNodes;
  plan->findNodesOfType(collectNodes, EN::COLLECT, /* enterSubqueries */ true);

  for (auto node : collectNodes) {
    TRI_ASSERT(node->getType() == EN::COLLECT);
    auto collectNode = ExecutionNode::castTo<CollectNode*>(node);
    LOG_RULE << "Found candidate collect node " << collectNode->id();

    if (not isCollectNodeEligible(*collectNode)) {
      continue;
    }

    // check is there is Index node as dependency
    auto dependency = collectNode->getFirstDependency();
    while (dependency != nullptr) {
      LOG_RULE << "DEPENDENCY " << dependency->id() << " "
               << dependency->getTypeString();
      if (dependency->getType() == EN::INDEX) {
        break;
      } else if (dependency->getType() == EN::CALCULATION) {
        auto calculation = EN::castTo<CalculationNode*>(dependency);
        LOG_RULE << "CALCULATION NODE " << calculation->id();
        dependency = dependency->getFirstDependency();
      } else {
        // maybe skip some nodes?
        dependency = nullptr;
      }
    }

    if (dependency == nullptr) {
      LOG_RULE << "No suitable dependency found";
      continue;
    }

    TRI_ASSERT(dependency->getType() == EN::INDEX);
    auto indexNode = ExecutionNode::castTo<IndexNode*>(dependency);
    LOG_RULE << "Found candidate index node " << indexNode->id();

    if (not isIndexNodeEligible(*indexNode)) {
      continue;
    }

    // TODO only call for non-aggregation
    if (not checkSelectivityFitsAlgorithm(*indexNode)) {
      continue;
    }

    auto singleton = indexNode->getFirstDependency();
    // This is required because this rule runs after distribute in cluster
    // rules, and we might be in a subquery of a query that starts with a
    // coordinator part. the remote node usually ends up between the index and
    // the subquery singleton node.
    while (singleton->getType() == EN::SCATTER ||
           singleton->getType() == EN::GATHER ||
           singleton->getType() == EN::REMOTE ||
           singleton->getType() == EN::DISTRIBUTE) {
      singleton = singleton->getFirstDependency();
    }
    if (singleton->getType() != EN ::SINGLETON) {
      LOG_RULE << "Index node is not the first node after a singleton.";
      continue;
    }

    LOG_RULE << "Found collect " << collectNode->id() << " with index "
             << indexNode->id();

    auto isEligibleResult = isEligiblePair(*plan, *collectNode, *indexNode);
    if (not isEligibleResult) {
      continue;
    }

    auto& [calculationsToRemove, groups, aggregations] = *isEligibleResult;

    auto indexCollectNode = plan->createNode<IndexCollectNode>(
        plan.get(), plan->nextId(), indexNode->collection(),
        indexNode->getSingleIndex(), indexNode->outVariable(),
        std::move(groups), std::move(aggregations), collectNode->getOptions());

    plan->replaceNode(collectNode, indexCollectNode);
    plan->unlinkNode(indexNode);
    for (auto n : calculationsToRemove) {
      plan->unlinkNode(n);
    }
    modified = true;
  }

  opt->addPlan(std::move(plan), rule, modified);
}

std::ostream& arangodb::operator<<(std::ostream& os,
                                   IndexDistinctScanOptions const& opts) {
  return os << opts.distinctFields << (opts.sorted ? "sorted" : "unsorted");
}
