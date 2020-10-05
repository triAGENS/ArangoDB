////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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

#include "ConditionFinder.h"

#include "Aql/Condition.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/IndexNode.h"
#include "Aql/SortCondition.h"
#include "Aql/SortNode.h"
#include "Basics/tryEmplaceHelper.h"


using namespace arangodb::aql;
using EN = arangodb::aql::ExecutionNode;

bool ConditionFinder::before(ExecutionNode* en) {
  switch (en->getType()) {
    case EN::READALL:
    case EN::ENUMERATE_LIST:
    case EN::COLLECT:
    case EN::SCATTER:
    case EN::DISTRIBUTE:
    case EN::GATHER:
    case EN::REMOTE:
    case EN::SUBQUERY:
    case EN::INDEX:
    case EN::RETURN:
    case EN::TRAVERSAL:
    case EN::K_SHORTEST_PATHS:
    case EN::SHORTEST_PATH:
    case EN::ENUMERATE_IRESEARCH_VIEW: {
      // in these cases we simply ignore the intermediate nodes, note
      // that we have taken care of nodes that could throw exceptions
      // above.
      break;
    }

    case EN::INSERT:
    case EN::REMOVE:
    case EN::REPLACE:
    case EN::UPDATE:
    case EN::UPSERT:
    case EN::LIMIT: {
      // LIMIT or modification invalidates the sort expression we already found
      _sorts.clear();
      _filters.clear();
      break;
    }

    case EN::SINGLETON:
    case EN::NORESULTS: {
      // in all these cases we better abort
      return true;
    }

    case EN::FILTER: {
      // register which variable is used in a FILTER
      _filters.emplace(
          ExecutionNode::castTo<FilterNode const*>(en)->inVariable()->id);
      break;
    }

    case EN::SORT: {
      // register which variables are used in a SORT
      if (_sorts.empty()) {
        for (auto& it : ExecutionNode::castTo<SortNode const*>(en)->elements()) {
          _sorts.emplace_back(it.var, it.ascending);
          TRI_IF_FAILURE("ConditionFinder::sortNode") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }
        }
      }
      break;
    }

    case EN::CALCULATION: {
      _variableDefinitions.try_emplace(
          ExecutionNode::castTo<CalculationNode const*>(en)->outVariable()->id,
          ExecutionNode::castTo<CalculationNode const*>(en)->expression()->node());
      TRI_IF_FAILURE("ConditionFinder::variableDefinition") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
      break;
    }

    case EN::ENUMERATE_COLLECTION: {
      auto node = ExecutionNode::castTo<EnumerateCollectionNode const*>(en);
      if (_changes->find(node->id()) != _changes->end()) {
        // already optimized this node
        break;
      }

      auto condition = std::make_unique<Condition>(_plan->getAst());
      bool ok = handleFilterCondition(en, condition);
      if (!ok) {
        break;
      }

      std::unique_ptr<SortCondition> sortCondition;
      handleSortCondition(en, node->outVariable(), condition, sortCondition);

      if (condition->isEmpty() && sortCondition->isEmpty()) {
        // no filter conditions left
        break;
      }

      std::vector<transaction::Methods::IndexHandle> usedIndexes;
      auto [filtering, sorting] = condition->findIndexes(node, usedIndexes, sortCondition.get());

      if (filtering || sorting) {
        bool descending = false;
        if (sorting && sortCondition->isUnidirectional()) {
          descending = sortCondition->isDescending();
        }

        if (!filtering) {
          // index cannot be used for filtering, but only for sorting
          // remove the condition now
          TRI_ASSERT(sorting);
          condition.reset(new Condition(_plan->getAst()));
          condition->normalize(_plan);
        }

        TRI_ASSERT(!usedIndexes.empty());

        // We either can find indexes for everything or findIndexes
        // will clear out usedIndexes
        IndexIteratorOptions opts;
        opts.ascending = !descending;
        TRI_IF_FAILURE("ConditionFinder::insertIndexNode") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        // We keep this node's change
        _changes->try_emplace(
            node->id(),
            arangodb::lazyConstruct([&]{
              IndexNode* idx = new IndexNode(_plan, _plan->nextId(), node->collection(),
                                             node->outVariable(), usedIndexes, std::move(condition), opts);
              // if the enumerate collection node had the counting flag
              // set, we can copy it over to the index node as well
              idx->copyCountFlag(node);
              return idx;
            })
        );
      }
      break;
    }

    default: {
      // should not reach this point
      TRI_ASSERT(false);
    }
  }

  return false;
}

bool ConditionFinder::enterSubquery(ExecutionNode*, ExecutionNode*) {
  return false;
}

bool ConditionFinder::handleFilterCondition(ExecutionNode* en,
                                            std::unique_ptr<Condition> const& condition) {
  bool foundCondition = false;

  for (auto& it : _variableDefinitions) {
    if (_filters.find(it.first) == _filters.end()) {
      continue;
    }

    // a variable used in a FILTER
    AstNode* var = const_cast<AstNode*>(it.second);
    if (var->isDeterministic() && var->isSimple()) {
      // replace all variables inside the FILTER condition with the
      // expressions represented by the variables
      var = it.second->clone(_plan->getAst());

      auto func = [&](AstNode* node) -> AstNode* {
        if (node->type == NODE_TYPE_REFERENCE) {
          auto variable = static_cast<Variable*>(node->getData());

          if (variable != nullptr) {
            auto setter = _plan->getVarSetBy(variable->id);

            if (setter != nullptr && setter->getType() == EN::CALCULATION) {
              auto s = ExecutionNode::castTo<CalculationNode*>(setter);
              auto filterExpression = s->expression();
              AstNode* inNode = filterExpression->nodeForModification();
              if (inNode->isDeterministic() && inNode->isSimple()) {
                return inNode;
              }
            }
          }
        }
        return node;
      };

      var = Ast::traverseAndModify(var, func);
    }

    condition->andCombine(var);
    foundCondition = true;
  }

  // normalize the condition
  condition->normalize(_plan);
  TRI_IF_FAILURE("ConditionFinder::normalizePlan") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  bool const conditionIsImpossible = (foundCondition && condition->isEmpty());

  if (conditionIsImpossible) {
    // condition is always false
    for (auto const& x : en->getParents()) {
      auto noRes = new NoResultsNode(_plan, _plan->nextId());
      _plan->registerNode(noRes);
      _plan->insertDependency(x, noRes);
      *_hasEmptyResult = true;
    }
    return false;
  }

  auto const& varsValid = en->getVarsValid();

  // remove all invalid variables from the condition
  if (condition->removeInvalidVariables(varsValid)) {
    // removing left a previously non-empty OR block empty...
    // this means we can't use the index to restrict the results
    return false;
  }
  return true;
}

void ConditionFinder::handleSortCondition(ExecutionNode* en, Variable const* outVar,
                                          std::unique_ptr<Condition> const& condition,
                                          std::unique_ptr<SortCondition>& sortCondition) {
  if (!en->isInInnerLoop()) {
    // we cannot optimize away a sort if we're in an inner loop ourselves
    sortCondition.reset(
        new SortCondition(_plan, _sorts, condition->getConstAttributes(outVar, false),
                          condition->getNonNullAttributes(outVar), _variableDefinitions));
  } else {
    sortCondition.reset(new SortCondition());
  }
}

ConditionFinder::ConditionFinder(ExecutionPlan* plan,
                                 std::unordered_map<ExecutionNodeId, ExecutionNode*>* changes,
                                 bool* hasEmptyResult, bool viewMode)
    : _plan(plan),
      _variableDefinitions(),
      _filters(),
      _sorts(),
      _changes(changes),
      _hasEmptyResult(hasEmptyResult) {}
