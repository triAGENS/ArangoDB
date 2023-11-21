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
#include "ExpressionMatcher.h"

#include <optional>
#include <variant>

#include "Aql/AstNode.h"
#include "Aql/ExecutionNode.h"
#include "Aql/Expression.h"
#include "Aql/EnumeratePathsNode.h"
#include "Aql/Ast.h"
#include "Aql/Quantifier.h"

#include "Basics/ErrorT.h"
#include "Basics/overload.h"

#include "Logger/LogMacros.h"
using EN = arangodb::aql::ExecutionNode;

namespace arangodb::aql::expression_matcher {
auto variable(std::string name) -> Variable { return Variable{.name = name}; }

}  // namespace arangodb::aql::expression_matcher
