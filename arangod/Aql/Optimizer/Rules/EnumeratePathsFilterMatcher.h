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
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Aql/Condition.h"
#include "Aql/ExecutionPlan.h"

#include "Logger/LogMacros.h"

namespace arangodb::aql {

/// @brief Walks the execution plan from leaf nodes upwards to find instances
/// of EnumeratePathsNodes whose path output variable `path` is filtered on
/// using a condition of the form
///
/// FOR p IN ENUMERATE_PATHS FROM v TO w GRAPH g
///   (?)
///   FILTER path.vertices[* ...] ALL == true
///   (?)
///   FILTER path.edges[* ...] ALL == true
///   (?)
///
struct EnumeratePathsFilterMatcher final
    : public WalkerWorker<ExecutionNode, WalkerUniqueness::NonUnique> {
 public:
  EnumeratePathsFilterMatcher(ExecutionPlan* plan);

  ~EnumeratePathsFilterMatcher() = default;

  bool before(ExecutionNode* node) override final;
  bool enterSubquery(ExecutionNode* node1, ExecutionNode* node2) override final;

 private:
  ExecutionPlan* _plan;
  std::unique_ptr<Condition> _condition;
  ::arangodb::containers::HashSet<VariableId> _filterVariables;
};

}  // namespace arangodb::aql
