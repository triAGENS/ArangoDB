////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/types.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>

namespace arangodb {
class Index;

namespace aql {

class Ast;
struct AstNode;
struct Collection;
class IndexHint;
class SortCondition;
struct Variable;
struct VarInfo;
struct NonConstExpression;
struct RegisterId;

/// code that used to be in transaction::Methods
namespace utils {

struct NonConstExpressionContainer {
  NonConstExpressionContainer() = default;
  ~NonConstExpressionContainer() = default;
  NonConstExpressionContainer(NonConstExpressionContainer const&) = delete;
  NonConstExpressionContainer(NonConstExpressionContainer&&) = default;
  NonConstExpressionContainer& operator=(NonConstExpressionContainer const&) = delete;

  std::vector<std::unique_ptr<NonConstExpression>> _expressions;
  std::unordered_map<VariableId, RegisterId> _varToRegisterMapping; 
  bool _hasV8Expression = false;

  // Serializes this container into a velocypack builder.
  void toVelocyPack(arangodb::velocypack::Builder& builder) const;
  static NonConstExpressionContainer fromVelocyPack(Ast* ast, arangodb::velocypack::Slice slice);

  NonConstExpressionContainer clone(Ast* ast) const;
};

/// @brief Gets the best fitting index for an AQL condition.
/// note: the caller must have read-locked the underlying collection when
/// calling this method
std::pair<bool, bool> getBestIndexHandlesForFilterCondition(
    aql::Collection const& coll, arangodb::aql::Ast* ast,
    arangodb::aql::AstNode* root, arangodb::aql::Variable const* reference,
    arangodb::aql::SortCondition const* sortCondition, size_t itemsInCollection,
    aql::IndexHint const& hint, std::vector<std::shared_ptr<Index>>& usedIndexes, bool& isSorted);

/// @brief Gets the best fitting index for an AQL condition.
/// note: the caller must have read-locked the underlying collection when
/// calling this method
bool getBestIndexHandleForFilterCondition(aql::Collection const& collection,
                                          arangodb::aql::AstNode*& node,
                                          arangodb::aql::Variable const* reference,
                                          size_t itemsInCollection,
                                          aql::IndexHint const& hint,
                                          std::shared_ptr<Index>& usedIndex,
                                          bool onlyEdgeIndexes = false);

/// @brief Gets the best fitting index for an AQL sort condition
/// note: the caller must have read-locked the underlying collection when
/// calling this method
bool getIndexForSortCondition(
    aql::Collection const& coll, arangodb::aql::SortCondition const* sortCondition,
    arangodb::aql::Variable const* reference, size_t itemsInIndex,
    aql::IndexHint const& hint, std::vector<std::shared_ptr<Index>>& usedIndexes,
    size_t& coveredAttributes);

NonConstExpressionContainer extractNonConstPartsOfIndexCondition(
    Ast* ast, std::unordered_map<VariableId, VarInfo> const& varInfo, bool evaluateFCalls,
    bool sorted, AstNode const* condition, Variable const* indexVariable);

} // namespace utils
} // namespace aql
} // namespace arangodb

