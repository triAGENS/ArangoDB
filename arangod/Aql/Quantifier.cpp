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

#include "Quantifier.h"

#include "Aql/AstNode.h"
#include "Basics/debugging.h"

#include <algorithm>

using namespace arangodb::aql;

/// @brief converts a quantifier string into an int equivalent
Quantifier::Type Quantifier::fromString(std::string_view value) {
  if (value == "all") {
    return Type::kAll;
  } else if (value == "any") {
    return Type::kAny;
  } else if (value == "none") {
    return Type::kNone;
  } else if (value == "at least") {
    return Type::kAtLeast;
  }

  TRI_ASSERT(false);
  return Type::kNone;
}

/// @brief converts a quantifier int value into its string equivalent
std::string_view Quantifier::stringify(Quantifier::Type type) {
  if (type == Type::kAll) {
    return "all";
  }
  if (type == Type::kAny) {
    return "any";
  }
  if (type == Type::kNone) {
    return "none";
  }
  if (type == Type::kAtLeast) {
    return "at least";
  }

  TRI_ASSERT(false);
  return "none";
}

bool Quantifier::isAll(AstNode const* quantifier) {
  return isType(quantifier, Type::kAll);
}

bool Quantifier::isAny(AstNode const* quantifier) {
  return isType(quantifier, Type::kAny);
}

bool Quantifier::isNone(AstNode const* quantifier) {
  return isType(quantifier, Type::kNone);
}

bool Quantifier::isAtLeast(AstNode const* quantifier) {
  return isType(quantifier, Type::kAtLeast);
}

std::optional<Quantifier::Type> Quantifier::getType(AstNode const* quantifier) {
  if (quantifier->type == NODE_TYPE_QUANTIFIER) {
    auto const value = quantifier->getIntValue(true);
    return static_cast<Type>(value);
  }
  return std::nullopt;
}

/// @brief determine the min/max number of matches for an array comparison
std::pair<size_t, size_t> Quantifier::requiredMatches(size_t inputSize,
                                                      AstNode const* quantifier,
                                                      size_t atLeastValue) {
  TRI_ASSERT(quantifier != nullptr);

  if (quantifier->type == NODE_TYPE_QUANTIFIER) {
    Type type = static_cast<Type>(quantifier->getIntValue(true));

    if (type == Quantifier::Type::kAll) {
      return {inputSize, std::max<size_t>(inputSize, 1)};
    }
    if (type == Quantifier::Type::kAny) {
      return {1, std::max<size_t>(1, inputSize)};
    }
    if (type == Quantifier::Type::kNone) {
      return {0, 0};
    }
    if (type == Quantifier::Type::kAtLeast) {
      TRI_ASSERT(quantifier->numMembers() == 1);
      return {atLeastValue, std::max(atLeastValue, inputSize)};
    }
  }

  // won't be reached
  TRI_ASSERT(false);
  return {SIZE_MAX, SIZE_MAX};
}

bool Quantifier::isType(AstNode const* quantifier, Type type) {
  TRI_ASSERT(quantifier != nullptr);

  if (quantifier->type == NODE_TYPE_QUANTIFIER) {
    auto const value = quantifier->getIntValue(true);
    return static_cast<Type>(value) == type;
  }
  return false;
}
