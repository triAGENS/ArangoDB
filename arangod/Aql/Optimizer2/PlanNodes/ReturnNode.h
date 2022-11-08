////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Markus Pfeiffer, Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Aql/Optimizer2/PlanNodes/BaseNode.h"
#include "Aql/Optimizer2/PlanNodeTypes/Variable.h"
#include "Aql/Optimizer2/PlanNodeTypes/Count.h"

#include <Inspection/VPackWithErrorT.h>

namespace arangodb::aql::optimizer2::nodes {

struct ReturnNode : optimizer2::nodes::BaseNode, optimizer2::types::Count {
  optimizer2::types::Variable inVariable;
};

template<typename Inspector>
auto inspect(Inspector& f, ReturnNode& x) {
  return f.object(x).fields(
      f.embedFields(static_cast<optimizer2::nodes::BaseNode&>(x)),
      f.embedFields(static_cast<optimizer2::types::Count&>(x)),
      f.field("inVariable", x.inVariable));
}

}  // namespace arangodb::aql::optimizer2::nodes
