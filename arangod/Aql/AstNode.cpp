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

#include "AstNode.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/Arithmetic.h"
#include "Aql/Ast.h"
#include "Aql/Function.h"
#include "Aql/Quantifier.h"
#include "Aql/Query.h"
#include "Aql/Scopes.h"
#include "Aql/types.h"
#include "Basics/FloatingPoint.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/fasthash.h"
#include "Containers/FlatHashSet.h"
#include "Transaction/Methods.h"

#include <array>
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#include <iostream>
#endif

#include <frozen/unordered_map.h>

#include <velocypack/Builder.h>
#include <velocypack/Dumper.h>
#include <velocypack/Iterator.h>
#include <velocypack/Sink.h>
#include <velocypack/Slice.h>
#include <velocypack/ValueType.h>

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb::aql {
namespace {

constexpr frozen::unordered_map<int, std::string_view, 26> kOperators{
    {static_cast<int>(NODE_TYPE_OPERATOR_UNARY_NOT), "!"},
    {static_cast<int>(NODE_TYPE_OPERATOR_UNARY_PLUS), "+"},
    {static_cast<int>(NODE_TYPE_OPERATOR_UNARY_MINUS), "-"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_AND), "&&"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_OR), "||"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_PLUS), "+"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_MINUS), "-"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_TIMES), "*"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_DIV), "/"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_MOD), "%"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_EQ), "=="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_NE), "!="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LT), "<"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LE), "<="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GT), ">"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GE), ">="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_IN), "IN"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_NIN), "NOT IN"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ), "array =="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_NE), "array !="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_LT), "array <"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_LE), "array <="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_GT), "array >"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_GE), "array >="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_IN), "array IN"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN), "array NOT IN"}};

/// @brief type names for AST nodes
frozen::unordered_map<int, std::string_view, 80> kTypeNames{
    {static_cast<int>(NODE_TYPE_ROOT), "root"},
    {static_cast<int>(NODE_TYPE_FOR), "for"},
    {static_cast<int>(NODE_TYPE_LET), "let"},
    {static_cast<int>(NODE_TYPE_FILTER), "filter"},
    {static_cast<int>(NODE_TYPE_RETURN), "return"},
    {static_cast<int>(NODE_TYPE_REMOVE), "remove"},
    {static_cast<int>(NODE_TYPE_INSERT), "insert"},
    {static_cast<int>(NODE_TYPE_UPDATE), "update"},
    {static_cast<int>(NODE_TYPE_REPLACE), "replace"},
    {static_cast<int>(NODE_TYPE_UPSERT), "upsert"},
    {static_cast<int>(NODE_TYPE_COLLECT), "collect"},
    {static_cast<int>(NODE_TYPE_SORT), "sort"},
    {static_cast<int>(NODE_TYPE_SORT_ELEMENT), "sort element"},
    {static_cast<int>(NODE_TYPE_LIMIT), "limit"},
    {static_cast<int>(NODE_TYPE_VARIABLE), "variable"},
    {static_cast<int>(NODE_TYPE_ASSIGN), "assign"},
    {static_cast<int>(NODE_TYPE_OPERATOR_UNARY_PLUS), "unary plus"},
    {static_cast<int>(NODE_TYPE_OPERATOR_UNARY_MINUS), "unary minus"},
    {static_cast<int>(NODE_TYPE_OPERATOR_UNARY_NOT), "unary not"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_AND), "logical and"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_OR), "logical or"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_PLUS), "plus"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_MINUS), "minus"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_TIMES), "times"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_DIV), "division"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_MOD), "modulus"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_EQ), "compare =="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_NE), "compare !="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LT), "compare <"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LE), "compare <="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GT), "compare >"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GE), "compare >="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_IN), "compare in"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_NIN), "compare not in"},
    {static_cast<int>(NODE_TYPE_OPERATOR_TERNARY), "ternary"},
    {static_cast<int>(NODE_TYPE_SUBQUERY), "subquery"},
    {static_cast<int>(NODE_TYPE_ATTRIBUTE_ACCESS), "attribute access"},
    {static_cast<int>(NODE_TYPE_BOUND_ATTRIBUTE_ACCESS),
     "bound attribute access"},
    {static_cast<int>(NODE_TYPE_INDEXED_ACCESS), "indexed access"},
    {static_cast<int>(NODE_TYPE_EXPANSION), "expansion"},
    {static_cast<int>(NODE_TYPE_ITERATOR), "iterator"},
    {static_cast<int>(NODE_TYPE_VALUE), "value"},
    {static_cast<int>(NODE_TYPE_ARRAY), "array"},
    {static_cast<int>(NODE_TYPE_OBJECT), "object"},
    {static_cast<int>(NODE_TYPE_OBJECT_ELEMENT), "object element"},
    {static_cast<int>(NODE_TYPE_COLLECTION), "collection"},
    {static_cast<int>(NODE_TYPE_REFERENCE), "reference"},
    {static_cast<int>(NODE_TYPE_PARAMETER), "parameter"},
    {static_cast<int>(NODE_TYPE_FCALL), "function call"},
    {static_cast<int>(NODE_TYPE_FCALL_USER), "user function call"},
    {static_cast<int>(NODE_TYPE_RANGE), "range"},
    {static_cast<int>(NODE_TYPE_NOP), "no-op"},
    {static_cast<int>(NODE_TYPE_CALCULATED_OBJECT_ELEMENT),
     "calculated object element"},
    {static_cast<int>(NODE_TYPE_EXAMPLE), "example"},
    {static_cast<int>(NODE_TYPE_PASSTHRU), "passthru"},
    {static_cast<int>(NODE_TYPE_ARRAY_LIMIT), "array limit"},
    {static_cast<int>(NODE_TYPE_DISTINCT), "distinct"},
    {static_cast<int>(NODE_TYPE_TRAVERSAL), "traversal"},
    {static_cast<int>(NODE_TYPE_DIRECTION), "direction"},
    {static_cast<int>(NODE_TYPE_COLLECTION_LIST), "collection list"},
    {static_cast<int>(NODE_TYPE_OPERATOR_NARY_AND), "n-ary and"},
    {static_cast<int>(NODE_TYPE_OPERATOR_NARY_OR), "n-ary or"},
    {static_cast<int>(NODE_TYPE_AGGREGATIONS), "aggregations array"},
    {static_cast<int>(NODE_TYPE_WITH), "with collections"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ), "array compare =="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_NE), "array compare !="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_LT), "array compare <"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_LE), "array compare <="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_GT), "array compare >"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_GE), "array compare >="},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_IN), "array compare in"},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN),
     "array compare not in"},
    {static_cast<int>(NODE_TYPE_QUANTIFIER), "quantifier"},
    {static_cast<int>(NODE_TYPE_SHORTEST_PATH), "shortest path"},
    {static_cast<int>(NODE_TYPE_ENUMERATE_PATHS), "enumerate paths"},
    {static_cast<int>(NODE_TYPE_VIEW), "view"},
    {static_cast<int>(NODE_TYPE_PARAMETER_DATASOURCE), "datasource parameter"},
    {static_cast<int>(NODE_TYPE_FOR_VIEW), "view enumeration"},
    {static_cast<int>(NODE_TYPE_ARRAY_FILTER), "array filter"},
    {static_cast<int>(NODE_TYPE_WINDOW), "window"},
};

/// @brief names for AST node value types
frozen::unordered_map<int, std::string_view, 5> kValueTypeNames{
    {static_cast<int>(VALUE_TYPE_NULL), "null"},
    {static_cast<int>(VALUE_TYPE_BOOL), "bool"},
    {static_cast<int>(VALUE_TYPE_INT), "int"},
    {static_cast<int>(VALUE_TYPE_DOUBLE), "double"},
    {static_cast<int>(VALUE_TYPE_STRING), "string"}};

/// @brief get the node type for inter-node comparisons
int valueTypeOrder(VPackValueType type) noexcept {
  switch (type) {
    case VPackValueType::Null:
      return 0;
    case VPackValueType::Bool:
      return 1;
    case VPackValueType::Int:
    case VPackValueType::Double:
      return 2;
    case VPackValueType::String:
    case VPackValueType::Custom:  // _id
      return 3;
    case VPackValueType::Array:
      return 4;
    case VPackValueType::Object:
      return 5;
    default:
      return 0;  // null
  }
}

/// @brief get the node type for inter-node comparisons
VPackValueType getNodeCompareType(AstNode const* node) noexcept {
  TRI_ASSERT(node != nullptr);

  /// quick translation array from an AST node value type to a VPack type
  constexpr std::array<VPackValueType, 5> kValueTypes{{
      VPackValueType::Null,    //    VALUE_TYPE_NULL   = 0,
      VPackValueType::Bool,    //    VALUE_TYPE_BOOL   = 1,
      VPackValueType::Int,     //    VALUE_TYPE_INT    = 2,
      VPackValueType::Double,  //    VALUE_TYPE_DOUBLE = 3,
      VPackValueType::String   //    VALUE_TYPE_STRING = 4
  }};

  if (node->type == NODE_TYPE_VALUE) {
    return kValueTypes[node->value.type];
  }
  if (node->type == NODE_TYPE_ARRAY) {
    return VPackValueType::Array;
  }
  if (node->type == NODE_TYPE_OBJECT) {
    return VPackValueType::Object;
  }

  // we should never get here
  TRI_ASSERT(false);

  // return null in case assertions are turned off
  return VPackValueType::Null;
}

int compareDoubleValues(double lhs, double rhs) {
  if (arangodb::almostEquals(lhs, rhs)) {
    return 0;
  }
  double diff = lhs - rhs;
  if (diff != 0.0) {
    return (diff < 0.0) ? -1 : 1;
  }
  return 0;
}

}  // namespace

static_assert(AstNodeValueType::VALUE_TYPE_NULL == 0,
              "incorrect ast node value types");
static_assert(AstNodeValueType::VALUE_TYPE_BOOL == 1,
              "incorrect ast node value types");
static_assert(AstNodeValueType::VALUE_TYPE_INT == 2,
              "incorrect ast node value types");
static_assert(AstNodeValueType::VALUE_TYPE_DOUBLE == 3,
              "incorrect ast node value types");
static_assert(AstNodeValueType::VALUE_TYPE_STRING == 4,
              "incorrect ast node value types");

/// @brief compare two nodes
/// @return range from -1 to +1 depending:
///  - -1 LHS being  less then   RHS,
///  -  0 LHS being     equal    RHS
///  -  1 LHS being greater then RHS
template<bool resolveAttributeAccess>
int compareAstNodes(AstNode const* lhs, AstNode const* rhs, bool compareUtf8) {
  TRI_ASSERT(lhs != nullptr);
  TRI_ASSERT(rhs != nullptr);

  bool lhsIsValid = true;
  bool rhsIsValid = true;

  if constexpr (resolveAttributeAccess) {
    if (lhs->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      lhs = Ast::resolveConstAttributeAccess(lhs, lhsIsValid);
    }
    if (rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      rhs = Ast::resolveConstAttributeAccess(rhs, rhsIsValid);
    }
  }
  VPackValueType const lType =
      lhsIsValid ? getNodeCompareType(lhs) : VPackValueType::Null;

  VPackValueType const rType =
      rhsIsValid ? getNodeCompareType(rhs) : VPackValueType::Null;

  if (lType != rType) {
    if (lType == VPackValueType::Int && rType == VPackValueType::Double) {
      // upcast int to double
      return compareDoubleValues(static_cast<double>(lhs->getIntValue()),
                                 rhs->getDoubleValue());
    } else if (lType == VPackValueType::Double &&
               rType == VPackValueType::Int) {
      // upcast int to double
      return compareDoubleValues(lhs->getDoubleValue(),
                                 static_cast<double>(rhs->getIntValue()));
    }

    int diff = valueTypeOrder(lType) - valueTypeOrder(rType);

    TRI_ASSERT(diff != 0);

    return (diff < 0) ? -1 : 1;
  }

  switch (lType) {
    case VPackValueType::Null: {
      return 0;
    }

    case VPackValueType::Bool: {
      int diff = static_cast<int>(lhs->getIntValue() - rhs->getIntValue());

      if (diff != 0) {
        return (diff < 0) ? -1 : 1;
      }
      return 0;
    }

    case VPackValueType::Int: {
      int64_t l = lhs->getIntValue();
      int64_t r = rhs->getIntValue();

      if (l != r) {
        return (l < r) ? -1 : 1;
      }
      return 0;
    }

    case VPackValueType::Double: {
      double diff = lhs->getDoubleValue() - rhs->getDoubleValue();
      if (diff != 0.0) {
        return (diff < 0.0) ? -1 : 1;
      }
      return 0;
    }

    case VPackValueType::String: {
      if (compareUtf8) {
        int res =
            TRI_compare_utf8(lhs->getStringValue(), lhs->getStringLength(),
                             rhs->getStringValue(), rhs->getStringLength());
        if (res != 0) {
          return res < 0 ? -1 : 1;
        }
        return 0;
      }

      size_t const minLength =
          (std::min)(lhs->getStringLength(), rhs->getStringLength());

      int res = memcmp(lhs->getStringValue(), rhs->getStringValue(), minLength);

      if (res != 0) {
        return res;
      }

      int diff = static_cast<int>(lhs->getStringLength()) -
                 static_cast<int>(rhs->getStringLength());

      if (diff != 0) {
        return diff < 0 ? -1 : 1;
      }
      return 0;
    }

    case VPackValueType::Array: {
      size_t const numLhs = lhs->numMembers();
      size_t const numRhs = rhs->numMembers();
      size_t const n = ((numLhs > numRhs) ? numRhs : numLhs);

      for (size_t i = 0; i < n; ++i) {
        int res = compareAstNodes<resolveAttributeAccess>(
            lhs->getMember(i), rhs->getMember(i), compareUtf8);

        if (res != 0) {
          return res;
        }
      }
      if (numLhs < numRhs) {
        return -1;
      } else if (numLhs > numRhs) {
        return 1;
      }
      return 0;
    }

    case VPackValueType::Object: {
      // this is a rather exceptional case, so we can
      // afford the inefficiency to convert the node to VPack
      // for comparison (this saves us from writing our own compare function
      // for array AstNodes)
      VPackBuilder builder;
      // add the first Slice to the Builder
      lhs->toVelocyPackValue(builder);

      // note the length of the first Slice
      VPackValueLength split = builder.size();

      // add the second Slice to the same Builder
      rhs->toVelocyPackValue(builder);

      return basics::VelocyPackHelper::compare(
          builder.slice() /*lhs*/, VPackSlice(builder.start() + split) /*rhs*/,
          compareUtf8);
    }

    default: {
      // all things equal
      return 0;
    }
  }
}

/// @brief create the node
AstNode::AstNode(AstNodeType type)
    : type(type), flags(0), _computedValue(nullptr) {
  // properly zero-initialize all members
  value.value._int = 0;
  value.length = 0;
  value.type = VALUE_TYPE_NULL;
}

/// @brief create the node, and make it an internal const node
AstNode::AstNode(AstNodeType type, InternalNode /*internal*/)
    : type(type),
      flags(makeFlags(DETERMINED_SORTED, DETERMINED_CONSTANT, DETERMINED_SIMPLE,
                      DETERMINED_NONDETERMINISTIC, DETERMINED_RUNONDBSERVER,
                      DETERMINED_CHECKUNIQUENESS, DETERMINED_V8,
                      VALUE_RUNONDBSERVER, FLAG_INTERNAL_CONST)),
      _computedValue(nullptr) {
  // properly zero-initialize all members
  value.value._int = 0;
  value.length = 0;
  value.type = VALUE_TYPE_NULL;
}

/// @brief create a node, with defining a value
AstNode::AstNode(AstNodeValue const& value)
    : type(NODE_TYPE_VALUE),
      flags(makeFlags(DETERMINED_CONSTANT, VALUE_CONSTANT, DETERMINED_SIMPLE,
                      VALUE_SIMPLE, DETERMINED_RUNONDBSERVER,
                      VALUE_RUNONDBSERVER)),
      value(value),
      _computedValue(nullptr) {}

/// @brief create a node, with defining a value and making it an
/// internal const node
AstNode::AstNode(AstNodeValue const& value, InternalNode /*internal*/)
    : type(NODE_TYPE_VALUE),
      flags(makeFlags(DETERMINED_SORTED, DETERMINED_CONSTANT, DETERMINED_SIMPLE,
                      DETERMINED_NONDETERMINISTIC, DETERMINED_RUNONDBSERVER,
                      DETERMINED_CHECKUNIQUENESS, DETERMINED_V8, VALUE_CONSTANT,
                      VALUE_SIMPLE, VALUE_RUNONDBSERVER, FLAG_INTERNAL_CONST)),
      value(value),
      _computedValue(nullptr) {}

/// @brief create the node from VPack
AstNode::AstNode(Ast* ast, arangodb::velocypack::Slice slice)
    : AstNode(getNodeTypeFromVPack(slice)) {
  TRI_ASSERT(flags == 0);
  TRI_ASSERT(_computedValue == nullptr);

  switch (type) {
    case NODE_TYPE_COLLECTION:
    case NODE_TYPE_VIEW:
    case NODE_TYPE_PARAMETER:
    case NODE_TYPE_PARAMETER_DATASOURCE:
    case NODE_TYPE_ATTRIBUTE_ACCESS:
    case NODE_TYPE_FCALL_USER: {
      value.type = VALUE_TYPE_STRING;
      VPackSlice v = slice.get("name");
      if (v.isNone()) {
        setStringValue(ast->resources().registerString("", 0), 0);
      } else {
        VPackValueLength l;
        char const* p = v.getString(l);
        setStringValue(ast->resources().registerString(p, l), l);
      }
      break;
    }
    case NODE_TYPE_VALUE: {
      int vType = slice.get("vTypeID").getNumericValue<int>();
      validateValueType(vType);
      value.type = static_cast<AstNodeValueType>(vType);

      switch (value.type) {
        case VALUE_TYPE_NULL:
          break;
        case VALUE_TYPE_BOOL:
          value.value._bool = slice.get("value").getBoolean();
          break;
        case VALUE_TYPE_INT:
          setIntValue(slice.get("value").getNumericValue<int64_t>());
          break;
        case VALUE_TYPE_DOUBLE:
          setDoubleValue(slice.get("value").getNumericValue<double>());
          break;
        case VALUE_TYPE_STRING: {
          VPackSlice v = slice.get("value");
          VPackValueLength l;
          char const* p = v.getString(l);
          setStringValue(ast->resources().registerString(p, l), l);
          break;
        }
        default: {
        }
      }
      break;
    }
    case NODE_TYPE_VARIABLE: {
      auto variable = ast->variables()->createVariable(slice);
      TRI_ASSERT(variable != nullptr);
      setData(variable);
      break;
    }
    case NODE_TYPE_REFERENCE: {
      auto variableId = slice.get("id").getNumericValue<VariableId>();
      auto variable = ast->variables()->getVariable(variableId);

      TRI_ASSERT(variable != nullptr);
      setData(variable);
      break;
    }
    case NODE_TYPE_FCALL: {
      auto& server = ast->query().vocbase().server();
      setData(server.getFeature<AqlFunctionFeature>().byName(
          slice.get("name").copyString()));
      break;
    }
    case NODE_TYPE_OBJECT_ELEMENT: {
      VPackSlice v = slice.get("name");
      VPackValueLength l;
      char const* p = v.getString(l);
      setStringValue(ast->resources().registerString(p, l), l);
      break;
    }
    case NODE_TYPE_EXPANSION: {
      setIntValue(slice.get("levels").getNumericValue<int64_t>());
      VPackSlice b = slice.get("booleanize");
      if (b.isBoolean() && b.getBoolean()) {
        setFlag(FLAG_BOOLEAN_EXPANSION);
      }
      break;
    }
    case NODE_TYPE_OPERATOR_BINARY_IN:
    case NODE_TYPE_OPERATOR_BINARY_NIN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_IN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN: {
      setBoolValue(slice.get("sorted").isTrue());
      break;
    }
    case NODE_TYPE_ARRAY: {
      if (VPackSlice v = slice.get("sorted"); v.isTrue()) {
        setFlag(DETERMINED_SORTED, VALUE_SORTED);
      }
      break;
    }
    case NODE_TYPE_QUANTIFIER: {
      setIntValue(static_cast<int64_t>(
          Quantifier::fromString(slice.get("quantifier").stringView())));
      break;
    }
    case NODE_TYPE_OPERATOR_BINARY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_LT:
    case NODE_TYPE_OPERATOR_BINARY_LE: {
      setExcludesNull(slice.get("excludesNull").isTrue());
      break;
    }
    case NODE_TYPE_OBJECT:
    case NODE_TYPE_ROOT:
    case NODE_TYPE_FOR:
    case NODE_TYPE_LET:
    case NODE_TYPE_FILTER:
    case NODE_TYPE_RETURN:
    case NODE_TYPE_REMOVE:
    case NODE_TYPE_INSERT:
    case NODE_TYPE_UPDATE:
    case NODE_TYPE_REPLACE:
    case NODE_TYPE_UPSERT:
    case NODE_TYPE_COLLECT:
    case NODE_TYPE_AGGREGATIONS:
    case NODE_TYPE_SORT:
    case NODE_TYPE_SORT_ELEMENT:
    case NODE_TYPE_LIMIT:
    case NODE_TYPE_ASSIGN:
    case NODE_TYPE_OPERATOR_UNARY_PLUS:
    case NODE_TYPE_OPERATOR_UNARY_MINUS:
    case NODE_TYPE_OPERATOR_UNARY_NOT:
    case NODE_TYPE_OPERATOR_BINARY_AND:
    case NODE_TYPE_OPERATOR_BINARY_OR:
    case NODE_TYPE_OPERATOR_BINARY_PLUS:
    case NODE_TYPE_OPERATOR_BINARY_MINUS:
    case NODE_TYPE_OPERATOR_BINARY_TIMES:
    case NODE_TYPE_OPERATOR_BINARY_DIV:
    case NODE_TYPE_OPERATOR_BINARY_MOD:
    case NODE_TYPE_OPERATOR_BINARY_NE:
    case NODE_TYPE_OPERATOR_BINARY_GT:
    case NODE_TYPE_OPERATOR_BINARY_GE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GE:
    case NODE_TYPE_OPERATOR_TERNARY:
    case NODE_TYPE_SUBQUERY:
    case NODE_TYPE_BOUND_ATTRIBUTE_ACCESS:
    case NODE_TYPE_INDEXED_ACCESS:
    case NODE_TYPE_ITERATOR:
    case NODE_TYPE_RANGE:
    case NODE_TYPE_NOP:
    case NODE_TYPE_CALCULATED_OBJECT_ELEMENT:
    case NODE_TYPE_EXAMPLE:
    case NODE_TYPE_PASSTHRU:
    case NODE_TYPE_ARRAY_LIMIT:
    case NODE_TYPE_DISTINCT:
    case NODE_TYPE_TRAVERSAL:
    case NODE_TYPE_SHORTEST_PATH:
    case NODE_TYPE_ENUMERATE_PATHS:
    case NODE_TYPE_DIRECTION:
    case NODE_TYPE_COLLECTION_LIST:
    case NODE_TYPE_OPERATOR_NARY_AND:
    case NODE_TYPE_OPERATOR_NARY_OR:
    case NODE_TYPE_WITH:
    case NODE_TYPE_FOR_VIEW:
    case NODE_TYPE_WINDOW:
    case NODE_TYPE_ARRAY_FILTER:
      break;
  }

  if (VPackSlice raw = slice.get("raw"); !raw.isNone()) {
    // hack: if there is a "raw" attribute, and we have either an array or
    // an object, it means that a special, more efficient/compact way of
    // encoding was chosen. we now have to decode this in a special way.
    if (type == NODE_TYPE_ARRAY) {
      // array
      VPackArrayIterator it(raw);
      members.reserve(it.size());
      while (it.valid()) {
        addMember(ast->nodeFromVPack(it.value(), /*copyStringValues*/ true));
        it.next();
      }
      return;
    } else if (type == NODE_TYPE_OBJECT) {
      // object
      VPackObjectIterator it(raw, /*useSequentialIteration*/ true);
      members.reserve(it.size());
      while (it.valid()) {
        std::string_view key = it.key().stringView();
        char* p = ast->resources().registerString(key);
        addMember(ast->createNodeObjectElement(
            std::string_view(p, key.size()),
            ast->nodeFromVPack(it.value(), /*copyStringValues*/ true)));
        it.next();
      }
      return;
    }
    // something else that had a "raw" attribute... 🥹
    TRI_ASSERT(false);
  }

  if (VPackSlice subNodes = slice.get("subNodes"); subNodes.isArray()) {
    members.reserve(subNodes.length());

    try {
      for (VPackSlice it : VPackArrayIterator(subNodes)) {
        int t = it.get("typeID").getNumericValue<int>();
        if (static_cast<AstNodeType>(t) == NODE_TYPE_NOP) {
          // special handling for nop as it is a singleton
          addMember(ast->createNodeNop());
        } else {
          addMember(ast->createNode(it));
        }
      }
    } catch (...) {
      // prevent leaks
      for (auto const& it : members) {
        if (it->type != NODE_TYPE_NOP) {
          delete it;
        }
      }
      throw;
    }
  }
}

/// @brief destroy the node
AstNode::~AstNode() { freeComputedValue(); }

/// @brief return the string value of a node, as an std::string
std::string AstNode::getString() const {
  TRI_ASSERT(
      type == NODE_TYPE_VALUE || type == NODE_TYPE_OBJECT_ELEMENT ||
      type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_PARAMETER ||
      type == NODE_TYPE_PARAMETER_DATASOURCE || type == NODE_TYPE_COLLECTION ||
      type == NODE_TYPE_VIEW || type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_FCALL_USER);
  TRI_ASSERT(value.type == VALUE_TYPE_STRING);
  return std::string(getStringValue(), getStringLength());
}

/// @brief return the string value of a node
std::string_view AstNode::getStringView() const noexcept {
  TRI_ASSERT(
      type == NODE_TYPE_VALUE || type == NODE_TYPE_OBJECT_ELEMENT ||
      type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_PARAMETER ||
      type == NODE_TYPE_PARAMETER_DATASOURCE || type == NODE_TYPE_COLLECTION ||
      type == NODE_TYPE_VIEW || type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_FCALL_USER);
  TRI_ASSERT(value.type == VALUE_TYPE_STRING);
  return std::string_view(getStringValue(), getStringLength());
}

/// @brief test if all members of a node are equality comparisons
bool AstNode::isOnlyEqualityMatch() const {
  if (type != NODE_TYPE_OPERATOR_BINARY_AND &&
      type != NODE_TYPE_OPERATOR_NARY_AND) {
    return false;
  }

  for (size_t i = 0; i < numMembers(); ++i) {
    auto op = getMemberUnchecked(i);
    if (op->type != arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
      return false;
    }
  }

  return true;
}

/// @brief computes a hash value for a value node
uint64_t AstNode::hashValue(uint64_t hash) const noexcept {
  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_NULL:
        return fasthash64(static_cast<void const*>("null"), 4, hash);
      case VALUE_TYPE_BOOL:
        if (value.value._bool) {
          return fasthash64(static_cast<void const*>("true"), 4, hash);
        }
        return fasthash64(static_cast<void const*>("false"), 5, hash);
      case VALUE_TYPE_INT:
        return fasthash64(static_cast<void const*>(&value.value._int),
                          sizeof(value.value._int), hash);
      case VALUE_TYPE_DOUBLE:
        return fasthash64(static_cast<void const*>(&value.value._double),
                          sizeof(value.value._double), hash);
      case VALUE_TYPE_STRING:
        return fasthash64(static_cast<void const*>(getStringValue()),
                          getStringLength(), hash);
    }
  }

  size_t const n = numMembers();

  if (type == NODE_TYPE_ARRAY) {
    hash = fasthash64(static_cast<void const*>("array"), 5, hash);
    for (size_t i = 0; i < n; ++i) {
      hash = getMemberUnchecked(i)->hashValue(hash);
    }
    return hash;
  }

  if (type == NODE_TYPE_OBJECT) {
    hash = fasthash64(static_cast<void const*>("object"), 6, hash);
    for (size_t i = 0; i < n; ++i) {
      auto sub = getMemberUnchecked(i);
      if (sub != nullptr) {
        hash = fasthash64(static_cast<void const*>(sub->getStringValue()),
                          sub->getStringLength(), hash);
        TRI_ASSERT(sub->numMembers() > 0);
        hash = sub->getMemberUnchecked(0)->hashValue(hash);
      }
    }
    return hash;
  }

  TRI_ASSERT(false);
  return 0;
}

/// @brief dump the node (for debugging purposes)
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
std::ostream& AstNode::toStream(std::ostream& os, int level) const {
  for (int i = 0; i < level; ++i) {
    os << "  ";
  }
  os << "- " << getTypeString();

  if (type == NODE_TYPE_VALUE || type == NODE_TYPE_ARRAY) {
    VPackBuilder b;
    toVelocyPackValue(b);
    os << ": " << b.toJson();
  } else if (type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    os << ": " << getString();
  } else if (type == NODE_TYPE_REFERENCE) {
    os << ": " << static_cast<Variable const*>(getData())->name;
  }
  os << "\n";

  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto sub = getMemberUnchecked(i);
    sub->toStream(os, level + 1);
  }
  return os;
}

void AstNode::dump(int indent) const { toStream(std::cout, indent); }
#endif

/// @brief compute the value for a constant value node
/// the value is owned by the node and must not be freed by the caller
VPackSlice AstNode::computeValue(VPackBuilder* builder) const {
  TRI_ASSERT(isConstant() || isStringValue());  // only strings could be mutable
  if (_computedValue == nullptr) {
    TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));

    if (builder == nullptr) {
      VPackBuilder b;
      computeValue(b);
    } else {
      builder->clear();
      computeValue(*builder);
    }
  }

  TRI_ASSERT(_computedValue != nullptr);

  return VPackSlice(_computedValue);
}

/// @brief internal function for actually computing the constant value
/// and storing it
void AstNode::computeValue(VPackBuilder& builder) const {
  TRI_ASSERT(builder.isEmpty());
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  toVelocyPackValue(builder);

  TRI_ASSERT(_computedValue == nullptr);
  _computedValue = new uint8_t[builder.size()];
  memcpy(_computedValue, builder.data(), builder.size());
}

/// @brief sort the members of an (array) node
/// this will also set the VALUE_SORTED flag for the node
void AstNode::sort() {
  TRI_ASSERT(type == NODE_TYPE_ARRAY);
  TRI_ASSERT(isConstant());
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));

  if (hasFlag(FLAG_BIND_PARAMETER)) {
    // specialized compare function that doesn't need to resolve
    // attribute accesses
    std::sort(members.begin(), members.end(),
              [](AstNode const* lhs, AstNode const* rhs) {
                return compareAstNodes<false>(lhs, rhs, true) < 0;
              });
  } else {
    // slower compare function that needs to resolve attribute
    // accesses
    std::sort(members.begin(), members.end(),
              [](AstNode const* lhs, AstNode const* rhs) {
                return compareAstNodes<true>(lhs, rhs, true) < 0;
              });
  }

  setFlag(DETERMINED_SORTED, VALUE_SORTED);
}

std::optional<std::string_view> AstNode::getTypeStringForType(
    AstNodeType type) {
  auto it = kTypeNames.find(static_cast<int>(type));

  if (it != kTypeNames.end()) {
    return (*it).second;
  }

  return std::nullopt;
}

/// @brief return the type name of a node
std::string_view AstNode::getTypeString() const {
  auto name = getTypeStringForType(type);

  if (name.has_value()) {
    return name.value();
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                 "missing node type in kTypeNames");
}

/// @brief return the value type name of a node
std::string_view AstNode::getValueTypeString() const {
  if (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT) {
    // actually the types ARRAY and OBJECT are no value types.
    // anyway, they need to be supported here because this function
    // can be called to determine the type of user-defined data for
    // error messages.
    auto it = kTypeNames.find(static_cast<int>(type));
    if (it != kTypeNames.end()) {
      return (*it).second;
    }
    // should not happen
    TRI_ASSERT(false);
  }
  auto it = kValueTypeNames.find(static_cast<int>(value.type));

  if (it != kValueTypeNames.end()) {
    return (*it).second;
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                 "missing node type in kValueTypeNames");
}

/// @brief stringify the AstNode
std::string AstNode::toString(AstNode const* node) {
  VPackBuilder builder;
  node->toVelocyPack(builder, false);
  return builder.toJson();
}

/// @brief checks whether we know a type of this kind; throws exception if not.
void AstNode::validateType(int type) {
  auto it = kTypeNames.find(static_cast<int>(type));

  if (it == kTypeNames.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "unknown AST node TypeID");
  }
}

/// @brief checks whether we know a value type of this kind;
/// throws exception if not.
void AstNode::validateValueType(int type) {
  auto it = kValueTypeNames.find(static_cast<int>(type));

  if (it == kValueTypeNames.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "invalid AST node valueTypeName");
  }
}

/// @brief fetch a node's type from VPack
AstNodeType AstNode::getNodeTypeFromVPack(arangodb::velocypack::Slice slice) {
  int type = slice.get("typeID").getNumericValue<int>();
  validateType(type);
  return static_cast<AstNodeType>(type);
}

void AstNode::setConstantFlags() noexcept {
  setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
  setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
  setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);
}

bool AstNode::valueHasVelocyPackRepresentation() const {
  switch (type) {
    case NODE_TYPE_VALUE:
      // All values are trivially representable as VPack
      return true;
    case NODE_TYPE_ARRAY: {
      // Is representable as VPack if all its members
      // are representable as VPack
      size_t const n = numMembers();
      for (size_t i = 0; i < n; ++i) {
        auto member = getMemberUnchecked(i);
        if (member != nullptr && !member->valueHasVelocyPackRepresentation()) {
          return false;
        }
      }
      return true;
    }
    case NODE_TYPE_OBJECT: {
      // Is representable as VPack if all its members
      // are representable as VPack
      size_t const n = numMembers();
      for (size_t i = 0; i < n; ++i) {
        auto member = getMemberUnchecked(i);
        if (member != nullptr) {
          // Throws if we do not have member 0
          auto value = member->getMember(0);
          TRI_ASSERT(value != nullptr);
          if (!value->valueHasVelocyPackRepresentation()) {
            return false;
          }
        }
      }
      return true;
    }
    case NODE_TYPE_ATTRIBUTE_ACCESS: {
      // Is representable as VPack if its first member is
      // representable as VPack
      auto value = getMember(0);
      TRI_ASSERT(value != nullptr);
      return value->valueHasVelocyPackRepresentation();
    }
    default:
      return false;
  }
}

/// @brief build a VelocyPack representation of the node value
///        Can throw Out of Memory Error
void AstNode::toVelocyPackValue(VPackBuilder& builder) const {
  TRI_ASSERT(valueHasVelocyPackRepresentation());
  if (type == NODE_TYPE_VALUE) {
    // dump value of "value" node
    switch (value.type) {
      case VALUE_TYPE_NULL:
        builder.add(VPackValue(VPackValueType::Null));
        break;
      case VALUE_TYPE_BOOL:
        builder.add(VPackValue(value.value._bool));
        break;
      case VALUE_TYPE_INT:
        builder.add(VPackValue(value.value._int));
        break;
      case VALUE_TYPE_DOUBLE:
        builder.add(VPackValue(value.value._double));
        break;
      case VALUE_TYPE_STRING:
        builder.add(VPackValuePair(value.value._string, value.length,
                                   VPackValueType::String));
        break;
    }
    return;
  }
  if (type == NODE_TYPE_ARRAY) {
    builder.openArray(/*allowUnindexed*/ false);
    size_t const n = numMembers();
    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);
      if (member != nullptr) {
        member->toVelocyPackValue(builder);
      }
    }
    builder.close();
    return;
  }

  if (type == NODE_TYPE_OBJECT) {
    builder.openObject();

    containers::FlatHashSet<std::string_view> keys;
    size_t const n = numMembers();

    // only check for duplicate keys if we have more than a single attribute
    bool checkUniqueness = (n > 1);
    if (checkUniqueness && hasFlag(DETERMINED_CHECKUNIQUENESS)) {
      // turn off duplicate keys checking if everything was already checked
      checkUniqueness = hasFlag(VALUE_CHECKUNIQUENESS);
    }

    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);
      if (member != nullptr) {
        std::string_view key(member->getStringView());

        if (checkUniqueness && !keys.emplace(key).second) {
          // duplicate key, skip it
          continue;
        }

        builder.add(VPackValue(key));
        member->getMember(0)->toVelocyPackValue(builder);
      }
    }
    builder.close();
    return;
  }

  if (type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    // TODO Could this be done more efficiently in the builder in place?
    VPackBuilder tmp;
    getMember(0)->toVelocyPackValue(tmp);

    VPackSlice slice = tmp.slice();
    if (slice.isObject()) {
      slice = slice.get(getStringView());
      if (!slice.isNone()) {
        builder.add(slice);
        return;
      }
    }
    builder.add(VPackValue(VPackValueType::Null));
  }

  // Do not add anything.
}

/// @brief create a VelocyPack representation of the node
void AstNode::toVelocyPack(VPackBuilder& builder, bool verbose) const {
  VPackObjectBuilder guard(&builder);

  // dump node type
  builder.add("type", VPackValue(getTypeString()));
  if (verbose) {
    builder.add("typeID", VPackValue(static_cast<int>(type)));
  }

  if (type == NODE_TYPE_COLLECTION || type == NODE_TYPE_VIEW ||
      type == NODE_TYPE_PARAMETER || type == NODE_TYPE_PARAMETER_DATASOURCE ||
      type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_OBJECT_ELEMENT ||
      type == NODE_TYPE_FCALL_USER) {
    // dump "name" of node
    TRI_ASSERT(getStringValue() != nullptr);
    builder.add("name", VPackValuePair(getStringValue(), getStringLength(),
                                       VPackValueType::String));
  }
  if (type == NODE_TYPE_FCALL) {
    auto func = static_cast<Function*>(getData());
    builder.add("name", VPackValue(func->name));
    // arguments are exported via node members
  }

  if (type == NODE_TYPE_ARRAY && hasFlag(DETERMINED_SORTED)) {
    // transport information about a node's sortedness
    builder.add("sorted", VPackValue(hasFlag(VALUE_SORTED)));
  }

  if (verbose && (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT) &&
      isConstant() && valueHasVelocyPackRepresentation()) {
    // this is a hack to serialize (potentially large) arrays and objects
    // in a more compact way.
    // instead of serializing every array/object member with their "typeID",
    // "vTypeID" etc. attributes, we simply store the array/object
    // data in plain JSON.
    // when unserializing the data back, we have to check if there is a "raw"
    // attribute, and if it exists, we have to use a special decoder for the
    // data.
    builder.add(VPackValue("raw"));
    toVelocyPackValue(builder);
    // do NOT descend into subnodes, because toVelocyPackValue has serialized
    // them already!
    return;
  }

  if (type == NODE_TYPE_VALUE) {
    // dump value of "value" node
    builder.add(VPackValue("value"));
    toVelocyPackValue(builder);

    if (verbose) {
      // it is not necessary to serialize the type name for the value type,
      // because there is not code that ever reads it back.
      // not serializing it helps to save a bit of overhead:
      //   builder.add("vType", VPackValue(getValueTypeString()));
      // we need to serialize the value type ID though, because that is
      // later read back
      builder.add("vTypeID", VPackValue(static_cast<int>(value.type)));
    }
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_EQ) {
    builder.add("excludesNull", VPackValue(getExcludesNull()));
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_NIN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
    builder.add("sorted", VPackValue(getBoolValue()));
  }
  if (type == NODE_TYPE_QUANTIFIER) {
    builder.add("quantifier",
                VPackValue(Quantifier::stringify(
                    static_cast<Quantifier::Type>(getIntValue(true)))));
  }

  if (type == NODE_TYPE_VARIABLE || type == NODE_TYPE_REFERENCE) {
    auto variable = static_cast<Variable*>(getData());

    TRI_ASSERT(variable != nullptr);
    builder.add("name", VPackValue(variable->name));
    builder.add("id", VPackValue(variable->id));
    if (type == NODE_TYPE_REFERENCE) {
      builder.add("subqueryReference",
                  VPackValue(hasFlag(FLAG_SUBQUERY_REFERENCE)));
    }
  }

  if (type == NODE_TYPE_EXPANSION) {
    builder.add("levels", VPackValue(getIntValue(true)));
    builder.add("booleanize", VPackValue(hasFlag(FLAG_BOOLEAN_EXPANSION)));
  }

  // dump sub-nodes
  if (size_t const n = members.size(); n > 0) {
    builder.add(VPackValue("subNodes"));
    builder.openArray(/*allowUnindexed*/ true);
    for (size_t i = 0; i < n; ++i) {
      AstNode* member = getMemberUnchecked(i);
      if (member != nullptr) {
        member->toVelocyPack(builder, verbose);
      }
    }
    builder.close();
  }
}

/// @brief iterates whether a node of type "searchType" can be found
bool AstNode::containsNodeType(AstNodeType searchType) const {
  if (type == searchType) {
    return true;
  }

  // iterate sub-nodes
  size_t const n = members.size();

  for (size_t i = 0; i < n; ++i) {
    AstNode* member = getMemberUnchecked(i);

    if (member != nullptr && member->containsNodeType(searchType)) {
      return true;
    }
  }

  return false;
}

/// @brief convert the node's value to a boolean value
/// this may create a new node or return the node itself if it is already a
/// boolean value node
AstNode const* AstNode::castToBool(Ast* ast) const {
  if (type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    return ast->resolveConstAttributeAccess(this)->castToBool(ast);
  }

  TRI_ASSERT(type == NODE_TYPE_VALUE || type == NODE_TYPE_ARRAY ||
             type == NODE_TYPE_OBJECT);

  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_NULL:
        // null => false
        return ast->createNodeValueBool(false);
      case VALUE_TYPE_BOOL:
        // already a boolean
        return this;
      case VALUE_TYPE_INT:
        return ast->createNodeValueBool(value.value._int != 0);
      case VALUE_TYPE_DOUBLE:
        return ast->createNodeValueBool(value.value._double != 0.0);
      case VALUE_TYPE_STRING:
        return ast->createNodeValueBool(value.length > 0);
      default: {
      }
    }
    // intentionally falls through
  } else if (type == NODE_TYPE_ARRAY) {
    return ast->createNodeValueBool(true);
  } else if (type == NODE_TYPE_OBJECT) {
    return ast->createNodeValueBool(true);
  }

  return ast->createNodeValueBool(false);
}

/// @brief convert the node's value to a number value
/// this may create a new node or return the node itself if it is already a
/// numeric value node
AstNode const* AstNode::castToNumber(Ast* ast) const {
  if (type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    return ast->resolveConstAttributeAccess(this)->castToNumber(ast);
  }

  TRI_ASSERT(type == NODE_TYPE_VALUE || type == NODE_TYPE_ARRAY ||
             type == NODE_TYPE_OBJECT);

  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_NULL:
        // null => 0
        return ast->createNodeValueInt(0);
      case VALUE_TYPE_BOOL:
        // false => 0, true => 1
        return ast->createNodeValueInt(value.value._bool ? 1 : 0);
      case VALUE_TYPE_INT:
      case VALUE_TYPE_DOUBLE:
        // already numeric!
        return this;
      case VALUE_TYPE_STRING: {
        bool failed;
        double v = arangodb::aql::stringToNumber(
            std::string(value.value._string, value.length), failed);
        if (failed) {
          return ast->createNodeValueInt(0);
        }
        return ast->createNodeValueDouble(v);
      }
        // intentionally falls through
    }
    // intentionally falls through
  } else if (type == NODE_TYPE_ARRAY) {
    size_t const n = numMembers();
    if (n == 0) {
      // [ ] => 0
      return ast->createNodeValueInt(0);
    } else if (n == 1) {
      auto member = getMember(0);
      // convert only member to number
      return member->castToNumber(ast);
    }
    // intentionally falls through
  } else if (type == NODE_TYPE_OBJECT) {
    // intentionally falls through
  }

  return ast->createNodeValueInt(0);
}

/// @brief get the integer value of a node, provided the node is a value node
/// this will return 0 for all non-value nodes and for all non-int value nodes!!
int64_t AstNode::getIntValue() const {
  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_BOOL:
        return (value.value._bool ? 1 : 0);
      case VALUE_TYPE_INT:
        return value.value._int;
      case VALUE_TYPE_DOUBLE:
        return static_cast<int64_t>(value.value._double);
      default: {
      }
    }
  }

  return 0;
}

/// @brief get the double value of a node, provided the node is a value node
double AstNode::getDoubleValue() const {
  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_BOOL:
        return (value.value._bool ? 1.0 : 0.0);
      case VALUE_TYPE_INT:
        return static_cast<double>(value.value._int);
      case VALUE_TYPE_DOUBLE:
        return value.value._double;
      default: {
      }
    }
  }

  return 0.0;
}

/// @brief whether or not the node value is trueish
bool AstNode::isTrue() const {
  if (type == NODE_TYPE_ATTRIBUTE_ACCESS && isConstant()) {
    bool isValid;
    AstNode const* resolved = Ast::resolveConstAttributeAccess(this, isValid);
    // TO_BOOL(null) => false, so isTrue(false) => false
    return isValid ? resolved->isTrue() : false;
  }

  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_NULL:
        return false;
      case VALUE_TYPE_BOOL:
        return value.value._bool;
      case VALUE_TYPE_INT:
        return (value.value._int != 0);
      case VALUE_TYPE_DOUBLE:
        return (value.value._double != 0.0);
      case VALUE_TYPE_STRING:
        return (value.length != 0);
    }
  } else if (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT) {
    return true;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_OR) {
    if (getMember(0)->isTrue()) {
      return true;
    }
    return getMember(1)->isTrue();
  } else if (type == NODE_TYPE_OPERATOR_BINARY_AND) {
    if (!getMember(0)->isTrue()) {
      return false;
    }
    return getMember(1)->isTrue();
  } else if (type == NODE_TYPE_OPERATOR_UNARY_NOT) {
    if (getMember(0)->isFalse()) {
      // ! false => true
      return true;
    }
  }

  return false;
}

/// @brief whether or not the node value is falsey
bool AstNode::isFalse() const {
  if (type == NODE_TYPE_ATTRIBUTE_ACCESS && isConstant()) {
    bool isValid;
    AstNode const* resolved = Ast::resolveConstAttributeAccess(this, isValid);
    // TO_BOOL(null) => false, so isFalse(false) => true
    return isValid ? resolved->isFalse() : true;
  }

  if (type == NODE_TYPE_VALUE) {
    switch (value.type) {
      case VALUE_TYPE_NULL:
        return true;
      case VALUE_TYPE_BOOL:
        return (!value.value._bool);
      case VALUE_TYPE_INT:
        return (value.value._int == 0);
      case VALUE_TYPE_DOUBLE:
        return (value.value._double == 0.0);
      case VALUE_TYPE_STRING:
        return (value.length == 0);
    }
  } else if (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT) {
    return false;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_OR) {
    if (!getMember(0)->isFalse()) {
      return false;
    }
    return getMember(1)->isFalse();
  } else if (type == NODE_TYPE_OPERATOR_BINARY_AND) {
    if (getMember(0)->isFalse()) {
      return true;
    }
    return getMember(1)->isFalse();
  } else if (type == NODE_TYPE_OPERATOR_UNARY_NOT) {
    if (getMember(0)->isTrue()) {
      // ! true => false
      return true;
    }
  }

  return false;
}

/// @brief whether or not a value node is of type attribute access that
/// refers to any variable reference
/// returns true if yes, and then also returns variable reference and array
/// of attribute names in the parameter passed by reference
bool AstNode::isAttributeAccessForVariable(
    std::pair<Variable const*, std::vector<arangodb::basics::AttributeName>>&
        result,
    bool allowIndexedAccess) const {
  if (!(type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_EXPANSION ||
        (allowIndexedAccess && type == NODE_TYPE_INDEXED_ACCESS))) {
    return false;
  }

  // initialize
  bool expandNext = false;
  result.first = nullptr;
  if (!result.second.empty()) {
    result.second.clear();
  }
  auto node = this;

  std::string indexBuffer;

  while (node->type == NODE_TYPE_ATTRIBUTE_ACCESS ||
         node->type == NODE_TYPE_INDEXED_ACCESS ||
         node->type == NODE_TYPE_EXPANSION) {
    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      arangodb::basics::AttributeName attr(node->getString(), expandNext);
      if (!indexBuffer.empty()) {
        attr.name.append(indexBuffer);
        indexBuffer.clear();
      }
      result.second.insert(result.second.begin(), std::move(attr));
      node = node->getMember(0);
      expandNext = false;
    } else if (node->type == NODE_TYPE_INDEXED_ACCESS) {
      TRI_ASSERT(node->numMembers() == 2);
      AstNode* val = node->getMemberUnchecked(1);
      if (!allowIndexedAccess || val->type != NODE_TYPE_VALUE) {
        break;  // also exclude all non trivial index accesses
      }
      indexBuffer.push_back('[');
      val->stringify(indexBuffer, false);
      indexBuffer.push_back(']');
      node = node->getMember(0);
      expandNext = false;
    } else {
      // expansion, i.e. [*]
      TRI_ASSERT(node->type == NODE_TYPE_EXPANSION);
      TRI_ASSERT(node->numMembers() >= 2);

      // check if the expansion uses a projection. if yes, we cannot use an
      // index for it
      if (node->getMember(4) != nullptr &&
          node->getMember(4)->type != NODE_TYPE_NOP) {
        // [* RETURN projection]
        result.second.clear();
        return false;
      }

      if (node->getMember(1)->type != NODE_TYPE_REFERENCE) {
        if (!node->getMember(1)->isAttributeAccessForVariable(
                result, allowIndexedAccess)) {
          result.second.clear();
          return false;
        }
      }
      expandNext = true;

      TRI_ASSERT(node->getMember(0)->type == NODE_TYPE_ITERATOR);

      node = node->getMember(0)->getMember(1);
    }
  }

  if (node->type != NODE_TYPE_REFERENCE) {
    result.second.clear();
    return false;
  }

  result.first = static_cast<Variable const*>(node->getData());
  return true;
}

/// @brief recursively clear flags
void AstNode::clearFlagsRecursive() noexcept {
  clearFlags();
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    getMemberUnchecked(i)->clearFlagsRecursive();
  }
}

/// @brief whether or not a node is simple enough to be used in a simple
/// expression
bool AstNode::isSimple() const {
  if (hasFlag(DETERMINED_SIMPLE)) {
    // fast track exit
    return hasFlag(VALUE_SIMPLE);
  }

  if (type == NODE_TYPE_REFERENCE || type == NODE_TYPE_VALUE ||
      type == NODE_TYPE_VARIABLE || type == NODE_TYPE_NOP ||
      type == NODE_TYPE_QUANTIFIER) {
    setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    return true;
  }

  if (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT ||
      type == NODE_TYPE_EXPANSION || type == NODE_TYPE_ITERATOR ||
      type == NODE_TYPE_ARRAY_LIMIT || type == NODE_TYPE_ARRAY_FILTER ||
      type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT ||
      type == NODE_TYPE_OPERATOR_TERNARY ||
      type == NODE_TYPE_OPERATOR_NARY_AND ||
      type == NODE_TYPE_OPERATOR_NARY_OR ||
      type == NODE_TYPE_OPERATOR_BINARY_PLUS ||
      type == NODE_TYPE_OPERATOR_BINARY_MINUS ||
      type == NODE_TYPE_OPERATOR_BINARY_TIMES ||
      type == NODE_TYPE_OPERATOR_BINARY_DIV ||
      type == NODE_TYPE_OPERATOR_BINARY_MOD ||
      type == NODE_TYPE_OPERATOR_BINARY_AND ||
      type == NODE_TYPE_OPERATOR_BINARY_OR ||
      type == NODE_TYPE_OPERATOR_BINARY_EQ ||
      type == NODE_TYPE_OPERATOR_BINARY_NE ||
      type == NODE_TYPE_OPERATOR_BINARY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_GT ||
      type == NODE_TYPE_OPERATOR_BINARY_GE ||
      type == NODE_TYPE_OPERATOR_BINARY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_NIN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NE ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GE ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN || type == NODE_TYPE_RANGE ||
      type == NODE_TYPE_INDEXED_ACCESS || type == NODE_TYPE_PASSTHRU ||
      type == NODE_TYPE_OBJECT_ELEMENT || type == NODE_TYPE_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_OPERATOR_UNARY_NOT ||
      type == NODE_TYPE_OPERATOR_UNARY_PLUS ||
      type == NODE_TYPE_OPERATOR_UNARY_MINUS) {
    size_t const n = numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);

      if (!member->isSimple()) {
        setFlag(DETERMINED_SIMPLE);
        return false;
      }
    }

    setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    return true;
  }

  if (type == NODE_TYPE_FCALL) {
    // some functions have C++ handlers
    // check if the called function is one of them
    auto func = static_cast<Function*>(getData());
    TRI_ASSERT(func != nullptr);

    TRI_ASSERT(numMembers() == 1);

    // check simplicity of function arguments
    auto args = getMember(0);
    size_t const n = args->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = args->getMemberUnchecked(i);
      auto conversion = func->getArgumentConversion(i);

      if (member->type == NODE_TYPE_COLLECTION &&
          (conversion == Function::Conversion::Required ||
           conversion == Function::Conversion::Optional)) {
        // collection attribute: no need to check for member simplicity
        continue;
      } else {
        // check for member simplicity the usual way
        if (!member->isSimple()) {
          setFlag(DETERMINED_SIMPLE);
          return false;
        }
      }
    }

    setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    return true;
  }

  if (type == NODE_TYPE_FCALL_USER) {
    setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    return true;
  }

  setFlag(DETERMINED_SIMPLE);
  return false;
}

/// @brief whether or not a node will use V8 internally
bool AstNode::willUseV8() const {
  if (hasFlag(DETERMINED_V8)) {
    // fast track exit
    return hasFlag(VALUE_V8);
  }

  if (type == NODE_TYPE_FCALL_USER) {
    // user-defined function will always use v8
    setFlag(DETERMINED_V8, VALUE_V8);
    return true;
  }

  if (type == NODE_TYPE_FCALL) {
    // some functions have C++ handlers
    // check if the called function is one of them
    auto func = static_cast<Function*>(getData());
    TRI_ASSERT(func != nullptr);

    if (func->hasV8Implementation()) {
      TRI_ASSERT(!func->hasCxxImplementation());
      // a function without a C++ implementation
      setFlag(DETERMINED_V8, VALUE_V8);
      return true;
    }

    if (func->name == "CALL" || func->name == "APPLY") {
      // CALL and APPLY can call arbitrary other functions...
      if (numMembers() > 0 && getMemberUnchecked(0)->isArray() &&
          getMemberUnchecked(0)->numMembers() > 0 &&
          getMemberUnchecked(0)->getMemberUnchecked(0)->isStringValue()) {
        // calling a function with a fixed name (known an query compile time)
        if (auto s =
                getMemberUnchecked(0)->getMemberUnchecked(0)->getStringView();
            s.find(':') != std::string::npos) {
          // a user-defined function.
          // this will use V8
          setFlag(DETERMINED_V8, VALUE_V8);
          return true;
        } else {
          // a built-in function (or some invalid function name)
          auto [normalized, isBuiltIn] = Ast::normalizeFunctionName(s);
          // note: normalizeFunctionName always returns an upper-case name.
          // we must hard-code the function name here, because we can't lookup
          // the definition for the function s without having access to the
          // AqlFunctionsFeature, which is not present here.
          if (normalized == "V8") {
            // the V8() function itself... obviously uses V8
            setFlag(DETERMINED_V8, VALUE_V8);
            return true;
          }
        }
        // fallthrough intentional. additionally inspect the function call
        // parameters for CALL/APPLY
      } else {
        // we are unsure about what function will be called by
        // CALL and APPLY. We cannot rule out user-defined functions,
        // so we assume the worst case here
        setFlag(DETERMINED_V8, VALUE_V8);
        return true;
      }
    }
  }

  // inspect all subnodes
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = getMemberUnchecked(i);

    if (member->willUseV8()) {
      setFlag(DETERMINED_V8, VALUE_V8);
      return true;
    }
  }

  setFlag(DETERMINED_V8);
  return false;
}

/// @brief whether or not a node's filter condition can be used inside a
/// TraversalNode
bool AstNode::canBeUsedInFilter(bool isOneShard) const {
  if (willUseV8() || !canRunOnDBServer(isOneShard) || !isDeterministic()) {
    return false;
  }
  return true;
}

/// @brief whether or not a node has a constant value
bool AstNode::isConstant() const {
  if (hasFlag(DETERMINED_CONSTANT)) {
    // fast track exit
    return hasFlag(VALUE_CONSTANT);
  }

  if (type == NODE_TYPE_VALUE) {
    setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
    return true;
  }

  if (type == NODE_TYPE_ARRAY) {
    size_t const n = numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);

      if (!member->isConstant()) {
        setFlag(DETERMINED_CONSTANT);
        return false;
      }
    }

    setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
    return true;
  }

  if (type == NODE_TYPE_OBJECT) {
    size_t const n = numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);
      if (member->type == NODE_TYPE_OBJECT_ELEMENT) {
        if (!member->getMember(0)->isConstant()) {
          setFlag(DETERMINED_CONSTANT);
          return false;
        }
      } else {
        // definitely not const!
        TRI_ASSERT(member->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT);
        setFlag(DETERMINED_CONSTANT);
        return false;
      }
    }

    setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
    return true;
  }

  if (type == NODE_TYPE_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS) {
    if (getMember(0)->isConstant()) {
      setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
      return true;
    }
  }

  if (type == NODE_TYPE_PARAMETER) {
    // bind parameter values will always be constant values later on...
    setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
    return true;
  }

  setFlag(DETERMINED_CONSTANT);
  return false;
}

/// @brief whether or not a node is a simple comparison operator
bool AstNode::isSimpleComparisonOperator() const {
  return (type == NODE_TYPE_OPERATOR_BINARY_EQ ||
          type == NODE_TYPE_OPERATOR_BINARY_NE ||
          type == NODE_TYPE_OPERATOR_BINARY_LT ||
          type == NODE_TYPE_OPERATOR_BINARY_LE ||
          type == NODE_TYPE_OPERATOR_BINARY_GT ||
          type == NODE_TYPE_OPERATOR_BINARY_GE);
}

/// @brief whether or not a node is a comparison operator
bool AstNode::isComparisonOperator() const {
  return (type == NODE_TYPE_OPERATOR_BINARY_EQ ||
          type == NODE_TYPE_OPERATOR_BINARY_NE ||
          type == NODE_TYPE_OPERATOR_BINARY_LT ||
          type == NODE_TYPE_OPERATOR_BINARY_LE ||
          type == NODE_TYPE_OPERATOR_BINARY_GT ||
          type == NODE_TYPE_OPERATOR_BINARY_GE ||
          type == NODE_TYPE_OPERATOR_BINARY_IN ||
          type == NODE_TYPE_OPERATOR_BINARY_NIN);
}

/// @brief whether or not a node is an array comparison operator
bool AstNode::isArrayComparisonOperator() const {
  return (type == NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NE ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LT ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LE ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GT ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GE ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN);
}

/// @brief whether or not a node (and its subnodes) can safely be executed on
/// a DB server
bool AstNode::canRunOnDBServer(bool isOneShard) const {
  if (hasFlag(DETERMINED_RUNONDBSERVER)) {
    // fast track exit
    return hasFlag(VALUE_RUNONDBSERVER);
  }

  // check sub-nodes first
  size_t const n = numMembers();
  for (size_t i = 0; i < n; ++i) {
    auto member = getMember(i);
    if (!member->canRunOnDBServer(isOneShard)) {
      // if any sub-node cannot run on a DB server, we can't either
      setFlag(DETERMINED_RUNONDBSERVER);
      return false;
    }
  }

  // now check ourselves
  if (type == NODE_TYPE_FCALL) {
    // built-in function
    auto func = static_cast<Function*>(getData());

    // currently being able to run on a DB server in cluster always includes
    // being able to run on a DB server in OneShard mode. this may change at
    // some point in the future.
    TRI_ASSERT(!func->hasFlag(Function::Flags::CanRunOnDBServerCluster) ||
               func->hasFlag(Function::Flags::CanRunOnDBServerOneShard));

    if ((isOneShard &&
         func->hasFlag(Function::Flags::CanRunOnDBServerOneShard)) ||
        (!isOneShard &&
         func->hasFlag(Function::Flags::CanRunOnDBServerCluster))) {
      setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);
      return true;
    }
    setFlag(DETERMINED_RUNONDBSERVER);
    return false;
  }

  if (type == NODE_TYPE_FCALL_USER) {
    // user function. we don't know anything about it
    setFlag(DETERMINED_RUNONDBSERVER);
    return false;
  }

  // everyhing else can run everywhere
  setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);
  return true;
}

/// @brief whether or not an object's keys must be checked for uniqueness
bool AstNode::mustCheckUniqueness() const {
  if (hasFlag(DETERMINED_CHECKUNIQUENESS)) {
    // fast track exit
    return hasFlag(VALUE_CHECKUNIQUENESS);
  }

  TRI_ASSERT(type == NODE_TYPE_OBJECT);

  bool mustCheck = false;

  // check the actual key members now
  if (size_t const n = numMembers(); n >= 2) {
    containers::FlatHashSet<std::string_view> keys;

    // only useful to check when there are 2 or more keys
    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);

      if (member->type == NODE_TYPE_OBJECT_ELEMENT) {
        // constant key
        if (!keys.emplace(member->getStringView()).second) {
          // duplicate key
          mustCheck = true;
          break;
        }
      } else if (member->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT) {
        // dynamic key... we don't know the key yet, so there's no
        // way around check it at runtime later
        mustCheck = true;
        break;
      }
    }
  }

  if (mustCheck) {
    // we have duplicate keys, or we can't tell yet
    setFlag(DETERMINED_CHECKUNIQUENESS, VALUE_CHECKUNIQUENESS);
  } else {
    // all keys unique, or just one or zero keys in object
    setFlag(DETERMINED_CHECKUNIQUENESS);
  }
  return mustCheck;
}

/// @brief whether or not a node (and its subnodes) is deterministic
bool AstNode::isDeterministic() const {
  if (hasFlag(DETERMINED_NONDETERMINISTIC)) {
    // fast track exit
    return !hasFlag(VALUE_NONDETERMINISTIC);
  }

  if (isConstant()) {
    return true;
  }

  // check sub-nodes first
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = getMemberUnchecked(i);

    if (!member->isDeterministic()) {
      // if any sub-node is non-deterministic, we are neither
      setFlag(DETERMINED_NONDETERMINISTIC, VALUE_NONDETERMINISTIC);
      return false;
    }
  }

  if (type == NODE_TYPE_FCALL) {
    // built-in functions may or may not be deterministic
    auto func = static_cast<Function*>(getData());

    if (!func->hasFlag(Function::Flags::Deterministic)) {
      setFlag(DETERMINED_NONDETERMINISTIC, VALUE_NONDETERMINISTIC);
      return false;
    }

    setFlag(DETERMINED_NONDETERMINISTIC);
    return true;
  }

  if (type == NODE_TYPE_FCALL_USER) {
    // user functions are always non-deterministic
    setFlag(DETERMINED_NONDETERMINISTIC, VALUE_NONDETERMINISTIC);
    return false;
  }

  // everything else is deterministic
  setFlag(DETERMINED_NONDETERMINISTIC);
  return true;
}

/// @brief whether or not a node (and its subnodes) is cacheable
bool AstNode::isCacheable() const {
  if (isConstant()) {
    return true;
  }

  // check sub-nodes first
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = getMemberUnchecked(i);

    if (!member->isCacheable()) {
      return false;
    }
  }

  if (type == NODE_TYPE_FCALL) {
    // built-in functions may or may not be cacheable
    auto func = static_cast<Function*>(getData());
    return func->hasFlag(Function::Flags::Cacheable);
  }

  // user functions are always non-cacheable.
  // everything else is cacheable
  return (type != NODE_TYPE_FCALL_USER);
}

/// @brief whether or not a node (and its subnodes) may contain a call to a
/// user-defined function
bool AstNode::callsUserDefinedFunction() const {
  if (isConstant()) {
    return false;
  }

  // check sub-nodes first
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = getMemberUnchecked(i);

    if (member->callsUserDefinedFunction()) {
      return true;
    }
  }

  return (type == NODE_TYPE_FCALL_USER);
}

/// @brief whether or not a node (and its subnodes) may contain a call to a
/// function or user-defined function
bool AstNode::callsFunction() const {
  if (isConstant()) {
    return false;
  }

  // check sub-nodes first
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = getMemberUnchecked(i);

    if (member->callsFunction()) {
      // abort early
      return true;
    }
  }

  return (type == NODE_TYPE_FCALL || type == NODE_TYPE_FCALL_USER);
}

/// @brief clone a node, recursively
AstNode* AstNode::clone(Ast* ast) const { return ast->clone(this); }

/// @brief append a string representation of the node to a string buffer
/// the string representation does not need to be JavaScript-compatible
/// except for node types NODE_TYPE_VALUE, NODE_TYPE_ARRAY and NODE_TYPE_OBJECT
/// (only for objects that do not contain dynamic attributes)
/// note that this may throw and that the caller is responsible for
/// catching the error
void AstNode::stringify(std::string& buffer, bool failIfLong) const {
  // any arrays/objects with more values than this will not be stringified if
  // failIfLong is set to true!
  constexpr size_t kTooLongThreshold = 80;

  if (type == NODE_TYPE_VALUE) {
    // must be JavaScript-compatible!
    appendValue(buffer);
    return;
  }

  arangodb::velocypack::StringSink sink(&buffer);
  arangodb::velocypack::Dumper dumper(&sink);

  if (type == NODE_TYPE_ARRAY) {
    // must be JavaScript-compatible!
    size_t const n = numMembers();

    if (failIfLong && n > kTooLongThreshold) {
      // intentionally do not stringify this node because the output would be
      // too long
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }

    buffer.push_back('[');
    for (size_t i = 0; i < n; ++i) {
      if (i > 0) {
        buffer.push_back(',');
      }

      AstNode* member = getMember(i);
      if (member != nullptr) {
        member->stringify(buffer, failIfLong);
      }
    }
    buffer.push_back(']');
    return;
  }

  if (type == NODE_TYPE_OBJECT) {
    // must be JavaScript-compatible!
    buffer.push_back('{');
    size_t const n = numMembers();

    if (failIfLong && n > kTooLongThreshold) {
      // intentionally do not stringify this node because the output would be
      // too long
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }

    for (size_t i = 0; i < n; ++i) {
      if (i > 0) {
        buffer.push_back(',');
      }

      AstNode* member = getMember(i);

      if (member->type == NODE_TYPE_OBJECT_ELEMENT) {
        TRI_ASSERT(member->numMembers() == 1);

        dumper.appendString(member->getStringValue(),
                            member->getStringLength());
        buffer.push_back(':');

        member->getMember(0)->stringify(buffer, failIfLong);
      } else if (member->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT) {
        TRI_ASSERT(member->numMembers() == 2);

        buffer.append("$[");
        member->getMember(0)->stringify(buffer, failIfLong);
        buffer.append("]:");
        member->getMember(1)->stringify(buffer, failIfLong);
      } else {
        TRI_ASSERT(false);
      }
    }
    buffer.push_back('}');
    return;
  }

  if (type == NODE_TYPE_REFERENCE || type == NODE_TYPE_VARIABLE) {
    // not used by V8
    auto variable = static_cast<Variable*>(getData());
    TRI_ASSERT(variable != nullptr);
    // we're intentionally not using the variable name as it is not necessarily
    // unique within a query (hey COLLECT, I am looking at you!)
    buffer.push_back('$');
    dumper.appendUInt(variable->id);
    return;
  }

  if (type == NODE_TYPE_INDEXED_ACCESS) {
    // not used by V8
    auto member = getMember(0);
    auto index = getMember(1);
    member->stringify(buffer, failIfLong);
    buffer.push_back('[');
    index->stringify(buffer, failIfLong);
    buffer.push_back(']');
    return;
  }

  if (type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    // not used by V8
    auto member = getMember(0);
    member->stringify(buffer, failIfLong);
    buffer.push_back('.');
    buffer.append(getStringValue(), getStringLength());
    return;
  }

  if (type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS) {
    // not used by V8
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back('.');
    getMember(1)->stringify(buffer, failIfLong);
    return;
  }

  if (type == NODE_TYPE_PARAMETER || type == NODE_TYPE_PARAMETER_DATASOURCE) {
    // not used by V8
    buffer.push_back('@');
    buffer.append(getStringValue(), getStringLength());
    return;
  }

  if (type == NODE_TYPE_FCALL) {
    // not used by V8
    auto func = static_cast<Function*>(getData());
    buffer.append(func->name);
    buffer.push_back('(');
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(')');
    return;
  }

  if (type == NODE_TYPE_ARRAY_LIMIT) {
    // not used by V8
    buffer.append("_LIMIT(");
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(',');
    getMember(1)->stringify(buffer, failIfLong);
    buffer.push_back(')');
    return;
  }

  if (type == NODE_TYPE_ARRAY_FILTER) {
    // not used by V8
    buffer.append("_FILTER(");
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(',');
    getMember(1)->stringify(buffer, failIfLong);
    buffer.push_back(')');
    return;
  }

  if (type == NODE_TYPE_EXPANSION) {
    // not used by V8
    buffer.append("_EXPANSION(");
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(',');
    getMember(1)->stringify(buffer, failIfLong);
    // filter
    buffer.push_back(',');

    auto filterNode = getMember(2);
    if (filterNode != nullptr && filterNode->type != NODE_TYPE_NOP) {
      buffer.append(" FILTER ");
      filterNode->getMember(0)->stringify(buffer, failIfLong);
    }
    auto limitNode = getMember(3);
    if (limitNode != nullptr && limitNode->type != NODE_TYPE_NOP) {
      buffer.append(" LIMIT ");
      limitNode->getMember(0)->stringify(buffer, failIfLong);
      buffer.push_back(',');
      limitNode->getMember(1)->stringify(buffer, failIfLong);
    }
    auto returnNode = getMember(4);
    if (returnNode != nullptr && returnNode->type != NODE_TYPE_NOP) {
      buffer.append(std::string_view(" RETURN "));
      returnNode->stringify(buffer, failIfLong);
    }

    buffer.push_back(')');
    return;
  }

  if (type == NODE_TYPE_NOP) {
    // not used by V8
    buffer.append("NOP");
    return;
  }

  if (type == NODE_TYPE_ITERATOR) {
    // not used by V8
    buffer.append("_ITERATOR(");
    getMember(1)->stringify(buffer, failIfLong);
    buffer.push_back(',');
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(')');
    return;
  }

  if (type == NODE_TYPE_OPERATOR_UNARY_NOT ||
      type == NODE_TYPE_OPERATOR_UNARY_PLUS ||
      type == NODE_TYPE_OPERATOR_UNARY_MINUS) {
    // not used by V8
    TRI_ASSERT(numMembers() == 1);
    auto it = kOperators.find(static_cast<int>(type));
    TRI_ASSERT(it != kOperators.end());
    buffer.push_back(' ');
    buffer.append((*it).second);

    getMember(0)->stringify(buffer, failIfLong);
    return;
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_AND ||
      type == NODE_TYPE_OPERATOR_BINARY_OR ||
      type == NODE_TYPE_OPERATOR_BINARY_PLUS ||
      type == NODE_TYPE_OPERATOR_BINARY_MINUS ||
      type == NODE_TYPE_OPERATOR_BINARY_TIMES ||
      type == NODE_TYPE_OPERATOR_BINARY_DIV ||
      type == NODE_TYPE_OPERATOR_BINARY_MOD ||
      type == NODE_TYPE_OPERATOR_BINARY_EQ ||
      type == NODE_TYPE_OPERATOR_BINARY_NE ||
      type == NODE_TYPE_OPERATOR_BINARY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_GT ||
      type == NODE_TYPE_OPERATOR_BINARY_GE ||
      type == NODE_TYPE_OPERATOR_BINARY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_NIN) {
    // not used by V8
    TRI_ASSERT(numMembers() == 2);
    auto it = kOperators.find(type);
    TRI_ASSERT(it != kOperators.end());

    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(' ');
    buffer.append((*it).second);
    buffer.push_back(' ');
    getMember(1)->stringify(buffer, failIfLong);
    return;
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NE ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GE ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
    // not used by V8
    TRI_ASSERT(numMembers() == 3);
    auto it = kOperators.find(type);
    TRI_ASSERT(it != kOperators.end());

    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back(' ');
    buffer.append(Quantifier::stringify(
        static_cast<Quantifier::Type>(getMember(2)->getIntValue(true))));
    buffer.push_back(' ');
    buffer.append((*it).second);
    buffer.push_back(' ');
    getMember(1)->stringify(buffer, failIfLong);
    return;
  }

  if (type == NODE_TYPE_OPERATOR_TERNARY) {
    getMember(0)->stringify(buffer, failIfLong);
    buffer.push_back('?');
    if (numMembers() == 3) {
      getMember(1)->stringify(buffer, failIfLong);
      buffer.push_back(':');
      getMember(2)->stringify(buffer, failIfLong);
    } else {
      buffer.push_back(':');
      getMember(1)->stringify(buffer, failIfLong);
    }
    return;
  }

  if (type == NODE_TYPE_OPERATOR_NARY_AND ||
      type == NODE_TYPE_OPERATOR_NARY_OR) {
    // not used by V8
    size_t const n = numMembers();
    for (size_t i = 0; i < n; ++i) {
      if (i > 0) {
        if (type == NODE_TYPE_OPERATOR_NARY_AND) {
          buffer.append(" AND ");
        } else {
          buffer.append(" OR ");
        }
      }
      getMember(i)->stringify(buffer, failIfLong);
    }
    return;
  }

  if (type == NODE_TYPE_RANGE) {
    // not used by V8
    TRI_ASSERT(numMembers() == 2);
    getMember(0)->stringify(buffer, failIfLong);
    buffer.append("..");
    getMember(1)->stringify(buffer, failIfLong);
    return;
  }

  std::string message("stringification not supported for node type ");
  message.append(getTypeString());

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, std::move(message));
}

/// note that this may throw and that the caller is responsible for
/// catching the error
std::string AstNode::toString() const {
  std::string buffer;
  stringify(buffer, false);
  return buffer;
}

/// @brief stringify the value of a node into a string buffer.
/// this creates an equivalent to what JSON.stringify() would do
void AstNode::appendValue(std::string& buffer) const {
  TRI_ASSERT(type == NODE_TYPE_VALUE);

  arangodb::velocypack::StringSink sink(&buffer);
  arangodb::velocypack::Dumper dumper(&sink);

  switch (value.type) {
    case VALUE_TYPE_BOOL: {
      if (value.value._bool) {
        buffer.append("true");
      } else {
        buffer.append("false");
      }
      break;
    }

    case VALUE_TYPE_INT: {
      dumper.appendInt(value.value._int);
      break;
    }

    case VALUE_TYPE_DOUBLE: {
      double const v = value.value._double;
      if (std::isnan(v) || !std::isfinite(v) || v == HUGE_VAL ||
          v == -HUGE_VAL) {
        buffer.append("null");
      } else {
        dumper.appendDouble(v);
      }
      break;
    }

    case VALUE_TYPE_STRING: {
      dumper.appendString(value.value._string, value.length);
      break;
    }

    case VALUE_TYPE_NULL:
    default: {
      buffer.append("null");
      break;
    }
  }
}

void AstNode::markFinalized(AstNode* subtreeRoot) noexcept {
  if ((nullptr == subtreeRoot) ||
      subtreeRoot->hasFlag(AstNodeFlagType::FLAG_FINALIZED)) {
    return;
  }

  subtreeRoot->setFlag(AstNodeFlagType::FLAG_FINALIZED);
  size_t const n = subtreeRoot->numMembers();
  for (size_t i = 0; i < n; ++i) {
    markFinalized(subtreeRoot->getMemberUnchecked(i));
  }
}

template<typename... Args>
std::underlying_type<AstNodeFlagType>::type AstNode::makeFlags(
    AstNodeFlagType flag, Args... args) noexcept {
  return static_cast<std::underlying_type<AstNodeFlagType>::type>(flag) +
         makeFlags(args...);
}

std::underlying_type<AstNodeFlagType>::type AstNode::makeFlags() noexcept {
  return static_cast<std::underlying_type<AstNodeFlagType>::type>(0);
}

bool AstNode::hasFlag(AstNodeFlagType flag) const noexcept {
  return ((flags & static_cast<decltype(flags)>(flag)) != 0);
}

void AstNode::clearFlags() noexcept {
  // clear all flags but these ones
  flags &= (AstNodeFlagType::FLAG_INTERNAL_CONST |
            AstNodeFlagType::FLAG_BOOLEAN_EXPANSION |
            AstNodeFlagType::FLAG_BIND_PARAMETER |
            AstNodeFlagType::FLAG_SUBQUERY_REFERENCE |
            AstNodeFlagType::FLAG_KEEP_VARIABLENAME);
}

void AstNode::setFlag(AstNodeFlagType flag) const noexcept { flags |= flag; }

void AstNode::setFlag(AstNodeFlagType typeFlag,
                      AstNodeFlagType valueFlag) const noexcept {
  flags |= (typeFlag | valueFlag);
}

void AstNode::removeFlag(AstNodeFlagType flag) const noexcept {
  flags &= ~flag;
}

bool AstNode::isSorted() const noexcept {
  return ((flags & (DETERMINED_SORTED | VALUE_SORTED)) ==
          (DETERMINED_SORTED | VALUE_SORTED));
}

bool AstNode::isNullValue() const noexcept {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_NULL);
}

bool AstNode::isIntValue() const noexcept {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_INT);
}

bool AstNode::isDoubleValue() const noexcept {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_DOUBLE);
}

bool AstNode::isNumericValue() const noexcept {
  return (type == NODE_TYPE_VALUE &&
          (value.type == VALUE_TYPE_INT || value.type == VALUE_TYPE_DOUBLE));
}

bool AstNode::isBoolValue() const noexcept {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_BOOL);
}

bool AstNode::isStringValue() const noexcept {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_STRING);
}

bool AstNode::isArray() const noexcept { return (type == NODE_TYPE_ARRAY); }

bool AstNode::isObject() const noexcept { return (type == NODE_TYPE_OBJECT); }

AstNode const* AstNode::getAttributeAccessForVariable(
    bool allowIndexedAccess) const {
  if (type != NODE_TYPE_ATTRIBUTE_ACCESS && type != NODE_TYPE_EXPANSION &&
      !(allowIndexedAccess && type == NODE_TYPE_INDEXED_ACCESS)) {
    return nullptr;
  }

  auto node = this;

  while (node->type == NODE_TYPE_ATTRIBUTE_ACCESS ||
         (allowIndexedAccess && node->type == NODE_TYPE_INDEXED_ACCESS) ||
         node->type == NODE_TYPE_EXPANSION) {
    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS ||
        node->type == NODE_TYPE_INDEXED_ACCESS) {
      node = node->getMember(0);
    } else {
      // expansion, i.e. [*]
      TRI_ASSERT(node->type == NODE_TYPE_EXPANSION);
      TRI_ASSERT(node->numMembers() >= 2);

      if (node->getMember(1)->type != NODE_TYPE_REFERENCE) {
        if (node->getMember(1)->getAttributeAccessForVariable(
                allowIndexedAccess) == nullptr) {
          return nullptr;
        }
      }

      TRI_ASSERT(node->getMember(0)->type == NODE_TYPE_ITERATOR);

      node = node->getMember(0)->getMember(1);
    }
  }

  if (node->type == NODE_TYPE_REFERENCE) {
    return node;
  }

  return nullptr;
}

bool AstNode::isAttributeAccessForVariable() const {
  return (getAttributeAccessForVariable(false) != nullptr);
}

bool AstNode::isAttributeAccessForVariable(Variable const* variable,
                                           bool allowIndexedAccess) const {
  auto node = getAttributeAccessForVariable(allowIndexedAccess);

  if (node == nullptr) {
    return false;
  }

  return (static_cast<Variable const*>(node->getData()) == variable);
}

size_t AstNode::numMembers() const noexcept { return members.size(); }

void AstNode::reserve(size_t n) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  members.reserve(n);
}

void AstNode::addMember(AstNode* node) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  if (node == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  try {
    members.emplace_back(node);
  } catch (...) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }
}

void AstNode::addMember(AstNode const* node) {
  addMember(const_cast<AstNode*>(node));
}

void AstNode::changeMember(size_t i, AstNode* node) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  if (i >= members.size()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "member out of range");
  }
  members[i] = node;
}

void AstNode::removeMemberUnchecked(size_t i) {
  TRI_ASSERT(members.size() > 0);
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  members.erase(members.begin() + i);
}

void AstNode::removeMemberUncheckedUnordered(size_t i) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  std::swap(members[i], members.back());
  members.pop_back();
}

AstNode* AstNode::getMember(size_t i) const {
  if (i >= members.size()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "member out of range");
  }
  return getMemberUnchecked(i);
}

AstNode* AstNode::getMemberUnchecked(size_t i) const noexcept {
  return members[i];
}

void AstNode::reduceMembers(size_t i) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  if (i > members.size()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "member out of range");
  }
  members.erase(members.begin() + i, members.end());
}

void AstNode::clearMembers() {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  members.clear();
}

void AstNode::setExcludesNull(bool v) {
  TRI_ASSERT(type == NODE_TYPE_OPERATOR_BINARY_LT ||
             type == NODE_TYPE_OPERATOR_BINARY_LE ||
             type == NODE_TYPE_OPERATOR_BINARY_EQ);
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  value.value._bool = v;
}

bool AstNode::getExcludesNull() const noexcept {
  TRI_ASSERT(type == NODE_TYPE_OPERATOR_BINARY_LT ||
             type == NODE_TYPE_OPERATOR_BINARY_LE ||
             type == NODE_TYPE_OPERATOR_BINARY_EQ);
  return value.value._bool;
}

void AstNode::setValueType(AstNodeValueType t) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  value.type = t;
}

bool AstNode::isValueType(AstNodeValueType expectedType) const noexcept {
  return value.type == expectedType;
}

bool AstNode::getBoolValue() const noexcept { return value.value._bool; }

void AstNode::setBoolValue(bool v) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));

  freeComputedValue();
  value.value._bool = v;
}

int64_t AstNode::getIntValue(bool) const noexcept { return value.value._int; }

void AstNode::setIntValue(int64_t v) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  freeComputedValue();
  value.value._int = v;
}

void AstNode::setDoubleValue(double v) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));

  freeComputedValue();
  value.value._double = v;
}

char const* AstNode::getStringValue() const { return value.value._string; }

size_t AstNode::getStringLength() const {
  return static_cast<size_t>(value.length);
}

void AstNode::setStringValue(char const* v, size_t length) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));

  freeComputedValue();
  // note: v may contain the NUL byte and is not necessarily
  // null-terminated itself (if from VPack)
  value.type = VALUE_TYPE_STRING;
  value.value._string = v;
  value.length = static_cast<uint32_t>(length);
}

bool AstNode::stringEqualsCaseInsensitive(std::string const& other) const {
  // Since we're not sure in how much trouble we are with unicode
  // strings, we assert here that strings we use are 7-bit ASCII
  TRI_ASSERT(std::none_of(other.begin(), other.end(),
                          [](const char c) { return c & 0x80; }));
  return (other.size() == getStringLength() &&
          strncasecmp(other.c_str(), getStringValue(), getStringLength()) == 0);
}

bool AstNode::stringEquals(std::string const& other) const {
  // Since we're not sure in how much trouble we are with unicode
  // strings, we assert here that strings we use are 7-bit ASCII
  TRI_ASSERT(std::none_of(other.begin(), other.end(),
                          [](const char c) { return c & 0x80; }));
  return (other.size() == getStringLength() &&
          memcmp(other.c_str(), getStringValue(), getStringLength()) == 0);
}

void* AstNode::getData() const noexcept { return value.value._data; }

void AstNode::setData(void* v) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));

  freeComputedValue();
  value.value._data = v;
}

void AstNode::setData(void const* v) { setData(const_cast<void*>(v)); }

void AstNode::freeComputedValue() noexcept {
  if (_computedValue != nullptr &&
      !hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST)) {
    delete[] _computedValue;
    _computedValue = nullptr;
  }
}

void AstNode::setComputedValue(uint8_t* data) {
  TRI_ASSERT(isConstant());
  TRI_ASSERT(hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  _computedValue = data;
}

/// @brief append the AstNode to an output stream
std::ostream& operator<<(std::ostream& stream,
                         arangodb::aql::AstNode const* node) {
  if (node != nullptr) {
    stream << arangodb::aql::AstNode::toString(node);
  }
  return stream;
}

/// @brief append the AstNode to an output stream
std::ostream& operator<<(std::ostream& stream,
                         arangodb::aql::AstNode const& node) {
  stream << arangodb::aql::AstNode::toString(&node);
  return stream;
}

AstNodeValue::Value::Value(int64_t value) : _int(value) {}

AstNodeValue::Value::Value(double value) : _double(value) {}

AstNodeValue::Value::Value(bool value) : _bool(value) {}

AstNodeValue::Value::Value(char const* value) : _string(value) {}

AstNodeValue::AstNodeValue()
    : value(int64_t(0)), length(0), type(VALUE_TYPE_NULL) {}

AstNodeValue::AstNodeValue(int64_t value)
    : value(value), length(0), type(VALUE_TYPE_INT) {}

AstNodeValue::AstNodeValue(double value)
    : value(value), length(0), type(VALUE_TYPE_DOUBLE) {}

AstNodeValue::AstNodeValue(bool value)
    : value(value), length(0), type(VALUE_TYPE_BOOL) {}

AstNodeValue::AstNodeValue(char const* value, uint32_t length)
    : value(value), length(length), type(VALUE_TYPE_STRING) {}

size_t AstNodeValueHash::operator()(AstNode const* value) const noexcept {
  return static_cast<size_t>(value->hashValue(0x12345678));
}

bool AstNodeValueEqual::operator()(AstNode const* lhs,
                                   AstNode const* rhs) const {
  return compareAstNodes(lhs, rhs, false) == 0;
}

// template instantiations for compareAstNodes
template int compareAstNodes<true>(AstNode const* lhs, AstNode const* rhs,
                                   bool compareUtf8);

template int compareAstNodes<false>(AstNode const* lhs, AstNode const* rhs,
                                    bool compareUtf8);

}  // namespace arangodb::aql
