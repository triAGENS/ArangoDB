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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Aql/Optimizer2/PlanNodes/BaseNode.h"

#include <Inspection/VPackWithErrorT.h>

namespace arangodb::aql::optimizer2::nodes {

struct LimitNode : optimizer2::nodes::BaseNode {
  AttributeTypes::Numeric offset;
  AttributeTypes::Numeric limit;
  bool fullCount;
};

template<typename Inspector>
auto inspect(Inspector& f, LimitNode& x) {
  return f.object(x).fields(
      f.embedFields(static_cast<optimizer2::nodes::BaseNode&>(x)),
      f.field("offset", x.offset), f.field("limit", x.limit),
      f.field("fullCount", x.fullCount));
}

}  // namespace arangodb::aql::optimizer2::nodes
