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

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <optional>

#include <Aql/AstNode.h>
#include <Aql/Condition.h>
#include <Aql/Variable.h>
#include <Aql/Quantifier.h>

#include <Aql/Optimizer/ExpressionMatcher/MatchResult.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchable.h>

#include <Aql/Optimizer/ExpressionMatcher/Matchers/Any.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/AnyValue.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/ArrayEq.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/AttributeAccess.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/Expansion.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/Into.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/Iterator.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NAryAnd.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NAryOr.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NoOp.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/NodeType.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/Quantifier.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/Reference.h>
#include <Aql/Optimizer/ExpressionMatcher/Matchers/Variable.h>
