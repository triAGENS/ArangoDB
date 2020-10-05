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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "AstNode.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/Arithmetic.h"
#include "Aql/Ast.h"
#include "Aql/Function.h"
#include "Aql/Quantifier.h"
#include "Aql/Query.h"
#include "Aql/Scopes.h"
#include "Aql/types.h"
#include "Basics/FloatingPoint.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/fasthash.h"
#include "Transaction/Methods.h"

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#include <iostream>
#endif

#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/StringRef.h>
#include <velocypack/ValueType.h>
#include <velocypack/velocypack-aliases.h>
#include <array>

using namespace arangodb;
using namespace arangodb::aql;

std::unordered_map<int, std::string const> const AstNode::Operators{
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
std::unordered_map<int, std::string const> const AstNode::TypeNames{
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
    {static_cast<int>(NODE_TYPE_COLLECT_COUNT), "collect with count"},
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
    {static_cast<int>(NODE_TYPE_K_SHORTEST_PATHS), "k-shortest paths"},
    {static_cast<int>(NODE_TYPE_VIEW), "view"},
    {static_cast<int>(NODE_TYPE_PARAMETER_DATASOURCE), "datasource parameter"},
    {static_cast<int>(NODE_TYPE_FOR_VIEW), "view enumeration"},
};

/// @brief names for AST node value types
std::unordered_map<int, std::string const> const AstNode::ValueTypeNames{
    {static_cast<int>(VALUE_TYPE_NULL), "null"},
    {static_cast<int>(VALUE_TYPE_BOOL), "bool"},
    {static_cast<int>(VALUE_TYPE_INT), "int"},
    {static_cast<int>(VALUE_TYPE_DOUBLE), "double"},
    {static_cast<int>(VALUE_TYPE_STRING), "string"}};

namespace {

/// @brief quick translation array from an AST node value type to a VPack type
std::array<VPackValueType, 5> const valueTypes{{
    VPackValueType::Null,    //    VALUE_TYPE_NULL   = 0,
    VPackValueType::Bool,    //    VALUE_TYPE_BOOL   = 1,
    VPackValueType::Int,     //    VALUE_TYPE_INT    = 2,
    VPackValueType::Double,  //    VALUE_TYPE_DOUBLE = 3,
    VPackValueType::String   //    VALUE_TYPE_STRING = 4
}};

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

/// @brief get the node type for inter-node comparisons
inline int valueTypeOrder(VPackValueType type) {
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
VPackValueType getNodeCompareType(AstNode const* node) {
  TRI_ASSERT(node != nullptr);

  if (node->type == NODE_TYPE_VALUE) {
    return valueTypes[node->value.type];
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

inline int compareDoubleValues(double lhs, double rhs) {
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

/// @brief compare two nodes
/// @return range from -1 to +1 depending:
///  - -1 LHS being  less then   RHS,
///  -  0 LHS being     equal    RHS
///  -  1 LHS being greater then RHS
int arangodb::aql::CompareAstNodes(AstNode const* lhs, AstNode const* rhs, bool compareUtf8) {
  TRI_ASSERT(lhs != nullptr);
  TRI_ASSERT(rhs != nullptr);

  bool isValid = true;
  if (lhs->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    lhs = Ast::resolveConstAttributeAccess(lhs, isValid);
  }
  VPackValueType const lType = isValid ? getNodeCompareType(lhs) : VPackValueType::Null;
  
  isValid = true;
  if (rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    rhs = Ast::resolveConstAttributeAccess(rhs, isValid);
  }
  VPackValueType const rType = isValid ? getNodeCompareType(rhs) : VPackValueType::Null;

  if (lType != rType) {
    if (lType == VPackValueType::Int && rType == VPackValueType::Double) {
      // upcast int to double
      return compareDoubleValues(static_cast<double>(lhs->getIntValue()),
                                 rhs->getDoubleValue());
    } else if (lType == VPackValueType::Double && rType == VPackValueType::Int) {
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
        int res = TRI_compare_utf8(lhs->getStringValue(), lhs->getStringLength(),
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
        int res = arangodb::aql::CompareAstNodes(lhs->getMember(i),
                                                 rhs->getMember(i), compareUtf8);

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
          builder.slice() /*lhs*/, 
          VPackSlice(builder.start() + split) /*rhs*/, 
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
    : type(type), flags(0), _computedValue(nullptr), members{} {
  // properly zero-initialize all members
  value.value._int = 0;
  value.length = 0;
  value.type = VALUE_TYPE_NULL;
}

/// @brief create a node, with defining a value
AstNode::AstNode(AstNodeValue const& value)
    : type(NODE_TYPE_VALUE),
      flags(makeFlags(DETERMINED_CONSTANT, VALUE_CONSTANT, DETERMINED_SIMPLE,
                      VALUE_SIMPLE, DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER)),
      value(value),
      _computedValue(nullptr),
      members{} {}

/// @brief create the node from VPack
AstNode::AstNode(Ast* ast, arangodb::velocypack::Slice const& slice)
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
      setData(AqlFunctionFeature::getFunctionByName(slice.get("name").copyString()));
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
      break;
    }
    case NODE_TYPE_OPERATOR_BINARY_IN:
    case NODE_TYPE_OPERATOR_BINARY_NIN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_IN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN: {
      bool sorted = false;
      VPackSlice v = slice.get("sorted");
      if (v.isBoolean()) {
        sorted = v.getBoolean();
      }
      setBoolValue(sorted);
      break;
    }
    case NODE_TYPE_ARRAY: {
      VPackSlice v = slice.get("sorted");
      if (v.isBoolean() && v.getBoolean()) {
        setFlag(DETERMINED_SORTED, VALUE_SORTED);
      }
      break;
    }
    case NODE_TYPE_QUANTIFIER: {
      setIntValue(Quantifier::FromString(slice.get("quantifier").copyString()));
      break;
    }
    case NODE_TYPE_OPERATOR_BINARY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_LT:
    case NODE_TYPE_OPERATOR_BINARY_LE: {
      bool excludesNull = false;
      VPackSlice v = slice.get("excludesNull");
      if (v.isBoolean()) {
        excludesNull = v.getBoolean();
      }
      setExcludesNull(excludesNull);
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
    case NODE_TYPE_COLLECT_COUNT:
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
    case NODE_TYPE_K_SHORTEST_PATHS:
    case NODE_TYPE_DIRECTION:
    case NODE_TYPE_COLLECTION_LIST:
    case NODE_TYPE_OPERATOR_NARY_AND:
    case NODE_TYPE_OPERATOR_NARY_OR:
    case NODE_TYPE_WITH:
    case NODE_TYPE_FOR_VIEW:
    case NODE_TYPE_READ_ALL:
      break;
  }

  VPackSlice subNodes = slice.get("subNodes");

  if (subNodes.isArray()) {
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

/// @brief create the node
AstNode::AstNode(std::function<void(AstNode*)> const& registerNode,
                 std::function<char const*(std::string const&)> registerString,
                 arangodb::velocypack::Slice const& slice)
    : AstNode(getNodeTypeFromVPack(slice)) {
  TRI_ASSERT(flags == 0);
  TRI_ASSERT(_computedValue == nullptr);

  switch (type) {
    case NODE_TYPE_ATTRIBUTE_ACCESS: {
      std::string const str(slice.get("name").copyString());
      value.type = VALUE_TYPE_STRING;
      auto p = registerString(str);
      TRI_ASSERT(p != nullptr);
      setStringValue(p, str.size());
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
          std::string const str(slice.get("value").copyString());
          setStringValue(registerString(str), str.size());
          break;
        }
        default: {
        }
      }
      break;
    }
    case NODE_TYPE_REFERENCE: {
      // We use the edge here
      break;
    }
    case NODE_TYPE_EXPANSION: {
      setIntValue(slice.get("levels").getNumericValue<int64_t>());
      break;
    }
    case NODE_TYPE_FCALL_USER:
    case NODE_TYPE_OBJECT_ELEMENT:
    case NODE_TYPE_COLLECTION:
    case NODE_TYPE_VIEW:
    case NODE_TYPE_PARAMETER:
    case NODE_TYPE_PARAMETER_DATASOURCE:
    case NODE_TYPE_VARIABLE:
    case NODE_TYPE_FCALL:
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
    case NODE_TYPE_COLLECT_COUNT:
    case NODE_TYPE_AGGREGATIONS:
    case NODE_TYPE_SORT:
    case NODE_TYPE_SORT_ELEMENT:
    case NODE_TYPE_LIMIT:
    case NODE_TYPE_ASSIGN:
    case NODE_TYPE_SUBQUERY:
    case NODE_TYPE_BOUND_ATTRIBUTE_ACCESS:
    case NODE_TYPE_CALCULATED_OBJECT_ELEMENT:
    case NODE_TYPE_EXAMPLE:
    case NODE_TYPE_DISTINCT:
    case NODE_TYPE_TRAVERSAL:
    case NODE_TYPE_SHORTEST_PATH:
    case NODE_TYPE_K_SHORTEST_PATHS:
    case NODE_TYPE_DIRECTION:
    case NODE_TYPE_COLLECTION_LIST:
    case NODE_TYPE_PASSTHRU:
    case NODE_TYPE_WITH:
    case NODE_TYPE_READ_ALL:
    case NODE_TYPE_FOR_VIEW: { 
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "Unsupported node type");
    }
    case NODE_TYPE_OBJECT:
    case NODE_TYPE_ROOT:
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
    case NODE_TYPE_OPERATOR_BINARY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_NE:
    case NODE_TYPE_OPERATOR_BINARY_LT:
    case NODE_TYPE_OPERATOR_BINARY_LE:
    case NODE_TYPE_OPERATOR_BINARY_GT:
    case NODE_TYPE_OPERATOR_BINARY_GE:
    case NODE_TYPE_OPERATOR_BINARY_IN:
    case NODE_TYPE_OPERATOR_BINARY_NIN:
    case NODE_TYPE_OPERATOR_TERNARY:
    case NODE_TYPE_INDEXED_ACCESS:
    case NODE_TYPE_ITERATOR:
    case NODE_TYPE_ARRAY:
    case NODE_TYPE_RANGE:
    case NODE_TYPE_NOP:
    case NODE_TYPE_ARRAY_LIMIT:
    case NODE_TYPE_OPERATOR_NARY_AND:
    case NODE_TYPE_OPERATOR_NARY_OR:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_IN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN:
    case NODE_TYPE_QUANTIFIER:
      break;
  }

  VPackSlice subNodes = slice.get("subNodes");

  if (subNodes.isArray()) {
    for (VPackSlice it : VPackArrayIterator(subNodes)) {
      addMember(new AstNode(registerNode, registerString, it));
    }
  }

  registerNode(this);
}

/// @brief destroy the node
AstNode::~AstNode() {
  freeComputedValue();
}

/// @brief return the string value of a node, as an std::string
std::string AstNode::getString() const {
  TRI_ASSERT(type == NODE_TYPE_VALUE || type == NODE_TYPE_OBJECT_ELEMENT ||
             type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_PARAMETER ||
             type == NODE_TYPE_PARAMETER_DATASOURCE || type == NODE_TYPE_COLLECTION ||
             type == NODE_TYPE_VIEW || type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS ||
             type == NODE_TYPE_FCALL_USER);
  TRI_ASSERT(value.type == VALUE_TYPE_STRING);
  return std::string(getStringValue(), getStringLength());
}

/// @brief return the string value of a node, as a arangodb::velocypack::StringRef
arangodb::velocypack::StringRef AstNode::getStringRef() const noexcept {
  TRI_ASSERT(type == NODE_TYPE_VALUE || type == NODE_TYPE_OBJECT_ELEMENT ||
             type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_PARAMETER ||
             type == NODE_TYPE_PARAMETER_DATASOURCE || type == NODE_TYPE_COLLECTION ||
             type == NODE_TYPE_VIEW || type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS ||
             type == NODE_TYPE_FCALL_USER);
  TRI_ASSERT(value.type == VALUE_TYPE_STRING);
  return arangodb::velocypack::StringRef(getStringValue(), getStringLength());
}

/// @brief test if all members of a node are equality comparisons
bool AstNode::isOnlyEqualityMatch() const {
  if (type != NODE_TYPE_OPERATOR_BINARY_AND && type != NODE_TYPE_OPERATOR_NARY_AND) {
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
  TRI_ASSERT(isConstant());

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

  std::sort(members.begin(), members.end(), [](AstNode const* lhs, AstNode const* rhs) {
    return (arangodb::aql::CompareAstNodes(lhs, rhs, true) < 0);
  });

  setFlag(DETERMINED_SORTED, VALUE_SORTED);
}

/// @brief return the type name of a node
std::string const& AstNode::getTypeString() const {
  auto it = TypeNames.find(static_cast<int>(type));

  if (it != TypeNames.end()) {
    return (*it).second;
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                 "missing node type in TypeNames");
}

/// @brief return the value type name of a node
std::string const& AstNode::getValueTypeString() const {
  if (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT) {
    // actually the types ARRAY and OBJECT are no value types.
    // anyway, they need to be supported here because this function
    // can be called to determine the type of user-defined data for
    // error messages.
    auto it = TypeNames.find(static_cast<int>(type));
    if (it != TypeNames.end()) {
      return (*it).second;
    }
    // should not happen
    TRI_ASSERT(false);
  }
  auto it = ValueTypeNames.find(static_cast<int>(value.type));

  if (it != ValueTypeNames.end()) {
    return (*it).second;
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                 "missing node type in ValueTypeNames");
}

/// @brief stringify the AstNode
std::string AstNode::toString(AstNode const* node) {
  VPackBuilder builder;
  node->toVelocyPack(builder, false);
  return builder.toJson();
}

/// @brief checks whether we know a type of this kind; throws exception if not.
void AstNode::validateType(int type) {
  auto it = TypeNames.find(static_cast<int>(type));

  if (it == TypeNames.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "unknown AST node TypeID");
  }
}

/// @brief checks whether we know a value type of this kind;
/// throws exception if not.
void AstNode::validateValueType(int type) {
  auto it = ValueTypeNames.find(static_cast<int>(type));

  if (it == ValueTypeNames.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                   "invalid AST node valueTypeName");
  }
}

/// @brief fetch a node's type from VPack
AstNodeType AstNode::getNodeTypeFromVPack(arangodb::velocypack::Slice const& slice) {
  int type = slice.get("typeID").getNumericValue<int>();
  validateType(type);
  return static_cast<AstNodeType>(type);
}

/// @brief build a VelocyPack representation of the node value
///        Can throw Out of Memory Error
void AstNode::toVelocyPackValue(VPackBuilder& builder) const {
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
        builder.add(VPackValuePair(value.value._string, value.length, VPackValueType::String));
        break;
    }
    return;
  }
  if (type == NODE_TYPE_ARRAY) {
    builder.openArray(false);
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

    std::unordered_set<VPackStringRef> keys;
    size_t const n = numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);
      if (member != nullptr) {
        VPackStringRef key(member->getStringValue(), member->getStringLength());

        if (n > 1 && !keys.emplace(key).second) {
          // duplicate key, skip it
          continue;
        }

        builder.add(VPackValuePair(member->getStringValue(),
                                   member->getStringLength(), VPackValueType::String));
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
      slice = slice.get(getString());
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
  if (type == NODE_TYPE_COLLECTION || type == NODE_TYPE_VIEW || type == NODE_TYPE_PARAMETER ||
      type == NODE_TYPE_PARAMETER_DATASOURCE || type == NODE_TYPE_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_OBJECT_ELEMENT || type == NODE_TYPE_FCALL_USER) {
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

  if (type == NODE_TYPE_VALUE) {
    // dump value of "value" node
    builder.add(VPackValue("value"));
    toVelocyPackValue(builder);

    if (verbose) {
      builder.add("vType", VPackValue(getValueTypeString()));
      builder.add("vTypeID", VPackValue(static_cast<int>(value.type)));
    }
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_EQ) {
    builder.add("excludesNull", VPackValue(getExcludesNull()));
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_IN || type == NODE_TYPE_OPERATOR_BINARY_NIN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
    builder.add("sorted", VPackValue(getBoolValue()));
  }
  if (type == NODE_TYPE_QUANTIFIER) {
    std::string const quantifier(Quantifier::Stringify(getIntValue(true)));
    builder.add("quantifier", VPackValue(quantifier));
  }

  if (type == NODE_TYPE_VARIABLE || type == NODE_TYPE_REFERENCE) {
    auto variable = static_cast<Variable*>(getData());

    TRI_ASSERT(variable != nullptr);
    builder.add("name", VPackValue(variable->name));
    builder.add("id", VPackValue(variable->id));
  }

  if (type == NODE_TYPE_EXPANSION) {
    builder.add("levels", VPackValue(getIntValue(true)));
  }

  // dump sub-nodes
  size_t const n = members.size();
  if (n > 0) {
    builder.add(VPackValue("subNodes"));
    builder.openArray(false);
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

  TRI_ASSERT(type == NODE_TYPE_VALUE || type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT);

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

  TRI_ASSERT(type == NODE_TYPE_VALUE || type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT);

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
        double v =
            arangodb::aql::stringToNumber(std::string(value.value._string, value.length), failed);
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
    std::pair<Variable const*, std::vector<arangodb::basics::AttributeName>>& result,
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

  basics::StringBuffer indexBuff;

  while (node->type == NODE_TYPE_ATTRIBUTE_ACCESS ||
         node->type == NODE_TYPE_INDEXED_ACCESS || node->type == NODE_TYPE_EXPANSION) {
    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      arangodb::basics::AttributeName attr(node->getString(), expandNext);
      if (indexBuff.length() > 0) {
        attr.name.append(indexBuff.c_str(), indexBuff.length());
        indexBuff.clear();
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
      indexBuff.appendChar('[');
      node->getMember(1)->stringify(&indexBuff, false, false);
      indexBuff.appendChar(']');
      node = node->getMember(0);
      expandNext = false;
    } else {
      // expansion, i.e. [*]
      TRI_ASSERT(node->type == NODE_TYPE_EXPANSION);
      TRI_ASSERT(node->numMembers() >= 2);

      // check if the expansion uses a projection. if yes, we cannot use an
      // index for it
      if (node->getMember(4) != nullptr && node->getMember(4)->type != NODE_TYPE_NOP) {
        // [* RETURN projection]
        result.second.clear();
        return false;
      }

      if (node->getMember(1)->type != NODE_TYPE_REFERENCE) {
        if (!node->getMember(1)->isAttributeAccessForVariable(result, allowIndexedAccess)) {
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
void AstNode::clearFlagsRecursive() {
  clearFlags();
  size_t const n = numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = getMemberUnchecked(i);
    member->clearFlagsRecursive();
  }
}

/// @brief whether or not a node is simple enough to be used in a simple
/// expression
bool AstNode::isSimple() const {
  if (hasFlag(DETERMINED_SIMPLE)) {
    // fast track exit
    return hasFlag(VALUE_SIMPLE);
  }

  if (type == NODE_TYPE_REFERENCE || type == NODE_TYPE_VALUE || type == NODE_TYPE_VARIABLE ||
      type == NODE_TYPE_NOP || type == NODE_TYPE_QUANTIFIER) {
    setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    return true;
  }

  if (type == NODE_TYPE_ARRAY || type == NODE_TYPE_OBJECT ||
      type == NODE_TYPE_EXPANSION || type == NODE_TYPE_ITERATOR ||
      type == NODE_TYPE_ARRAY_LIMIT || type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT ||
      type == NODE_TYPE_OPERATOR_TERNARY || type == NODE_TYPE_OPERATOR_NARY_AND ||
      type == NODE_TYPE_OPERATOR_NARY_OR || type == NODE_TYPE_OPERATOR_BINARY_PLUS ||
      type == NODE_TYPE_OPERATOR_BINARY_MINUS || type == NODE_TYPE_OPERATOR_BINARY_TIMES ||
      type == NODE_TYPE_OPERATOR_BINARY_DIV || type == NODE_TYPE_OPERATOR_BINARY_MOD ||
      type == NODE_TYPE_OPERATOR_BINARY_AND || type == NODE_TYPE_OPERATOR_BINARY_OR ||
      type == NODE_TYPE_OPERATOR_BINARY_EQ || type == NODE_TYPE_OPERATOR_BINARY_NE ||
      type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_GT || type == NODE_TYPE_OPERATOR_BINARY_GE ||
      type == NODE_TYPE_OPERATOR_BINARY_IN || type == NODE_TYPE_OPERATOR_BINARY_NIN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN || type == NODE_TYPE_RANGE ||
      type == NODE_TYPE_INDEXED_ACCESS || type == NODE_TYPE_PASSTHRU ||
      type == NODE_TYPE_OBJECT_ELEMENT || type == NODE_TYPE_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS || type == NODE_TYPE_OPERATOR_UNARY_NOT ||
      type == NODE_TYPE_OPERATOR_UNARY_PLUS || type == NODE_TYPE_OPERATOR_UNARY_MINUS) {
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
    
    if (func->implementation == nullptr) {
      // a function without a C++ implementation
      setFlag(DETERMINED_V8, VALUE_V8);
      return true;
    }
    
    if (func->name == "CALL" || func->name == "APPLY") {
      // CALL and APPLY can call arbitrary other functions...
      if (numMembers() > 0 && getMemberUnchecked(0)->isStringValue()) {
        auto s = getMemberUnchecked(0)->getStringRef();
        if (s.find(':') != std::string::npos) {
          // a user-defined function.
          // this will use V8
          setFlag(DETERMINED_V8, VALUE_V8);
          return true;
        }
        // fallthrough intentional
      } else {
        // we are unsure about what function will be called by 
        // CALL and APPLY. We cannot rule out user-defined functions,
        // so we assume the worst case here
        setFlag(DETERMINED_V8, VALUE_V8);
        return true;
      }
    }

  }

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
        auto value = member->getMember(0);

        if (!value->isConstant()) {
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

  if (type == NODE_TYPE_ATTRIBUTE_ACCESS || type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS) {
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
  return (type == NODE_TYPE_OPERATOR_BINARY_EQ || type == NODE_TYPE_OPERATOR_BINARY_NE ||
          type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
          type == NODE_TYPE_OPERATOR_BINARY_GT || type == NODE_TYPE_OPERATOR_BINARY_GE);
}

/// @brief whether or not a node is a comparison operator
bool AstNode::isComparisonOperator() const {
  return (type == NODE_TYPE_OPERATOR_BINARY_EQ || type == NODE_TYPE_OPERATOR_BINARY_NE ||
          type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
          type == NODE_TYPE_OPERATOR_BINARY_GT || type == NODE_TYPE_OPERATOR_BINARY_GE ||
          type == NODE_TYPE_OPERATOR_BINARY_IN || type == NODE_TYPE_OPERATOR_BINARY_NIN);
}

/// @brief whether or not a node is an array comparison operator
bool AstNode::isArrayComparisonOperator() const {
  return (type == NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LT ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GT ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
          type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN);
}

/// @brief whether or not a node (and its subnodes) can safely be executed on
/// a DB server
bool AstNode::canRunOnDBServer() const {
  if (hasFlag(DETERMINED_RUNONDBSERVER)) {
    // fast track exit
    return hasFlag(VALUE_RUNONDBSERVER);
  }

  // check sub-nodes first
  size_t const n = numMembers();
  for (size_t i = 0; i < n; ++i) {
    auto member = getMember(i);
    if (!member->canRunOnDBServer()) {
      // if any sub-node cannot run on a DB server, we can't either
      setFlag(DETERMINED_RUNONDBSERVER);
      return false;
    }
  }

  // now check ourselves
  if (type == NODE_TYPE_FCALL) {
    // built-in function
    auto func = static_cast<Function*>(getData());
    if (func->hasFlag(Function::Flags::CanRunOnDBServer)) {
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
  size_t const n = numMembers();

  if (n >= 2) {
    std::unordered_set<std::string> keys;

    // only useful to check when there are 2 or more keys
    for (size_t i = 0; i < n; ++i) {
      auto member = getMemberUnchecked(i);

      if (member->type == NODE_TYPE_OBJECT_ELEMENT) {
        // constant key
        if (!keys.emplace(member->getString()).second) {
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

  if (type == NODE_TYPE_FCALL_USER) {
    // user functions are always non-cacheable
    return false;
  }

  // everything else is cacheable
  return true;
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

/// @brief whether or not the object node contains dynamically named attributes
/// on its first level
bool AstNode::containsDynamicAttributeName() const {
  TRI_ASSERT(type == NODE_TYPE_OBJECT);

  size_t const n = numMembers();
  for (size_t i = 0; i < n; ++i) {
    if (getMember(i)->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT) {
      return true;
    }
  }

  return false;
}

/// @brief clone a node, recursively
AstNode* AstNode::clone(Ast* ast) const { return ast->clone(this); }

/// @brief validate that given node is an object with const-only values
bool AstNode::isConstObject() const {
  if (type != NODE_TYPE_OBJECT) {
    return false;
  }

  return isConstant();
}

/// @brief append a string representation of the node to a string buffer
/// the string representation does not need to be JavaScript-compatible
/// except for node types NODE_TYPE_VALUE, NODE_TYPE_ARRAY and NODE_TYPE_OBJECT
/// (only for objects that do not contain dynamic attributes)
/// note that this may throw and that the caller is responsible for
/// catching the error
void AstNode::stringify(arangodb::basics::StringBuffer* buffer, bool verbose,
                        bool failIfLong) const {
  // any arrays/objects with more values than this will not be stringified if
  // failIfLong is set to true!
  static size_t const TooLongThreshold = 80;

  if (type == NODE_TYPE_VALUE) {
    // must be JavaScript-compatible!
    appendValue(buffer);
    return;
  }

  if (type == NODE_TYPE_ARRAY) {
    // must be JavaScript-compatible!
    size_t const n = numMembers();

    if (failIfLong && n > TooLongThreshold) {
      // intentionally do not stringify this node because the output would be
      // too long
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }

    buffer->appendChar('[');
    for (size_t i = 0; i < n; ++i) {
      if (i > 0) {
        buffer->appendChar(',');
      }

      AstNode* member = getMember(i);
      if (member != nullptr) {
        member->stringify(buffer, verbose, failIfLong);
      }
    }
    buffer->appendChar(']');
    return;
  }

  if (type == NODE_TYPE_OBJECT) {
    // must be JavaScript-compatible!
    buffer->appendChar('{');
    size_t const n = numMembers();

    if (failIfLong && n > TooLongThreshold) {
      // intentionally do not stringify this node because the output would be
      // too long
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }

    for (size_t i = 0; i < n; ++i) {
      if (i > 0) {
        buffer->appendChar(',');
      }

      AstNode* member = getMember(i);

      if (member->type == NODE_TYPE_OBJECT_ELEMENT) {
        TRI_ASSERT(member->numMembers() == 1);

        buffer->appendJsonEncoded(member->getStringValue(), member->getStringLength());
        buffer->appendChar(':');

        member->getMember(0)->stringify(buffer, verbose, failIfLong);
      } else if (member->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT) {
        TRI_ASSERT(member->numMembers() == 2);

        buffer->appendText(TRI_CHAR_LENGTH_PAIR("$["));
        member->getMember(0)->stringify(buffer, verbose, failIfLong);
        buffer->appendText(TRI_CHAR_LENGTH_PAIR("]:"));
        member->getMember(1)->stringify(buffer, verbose, failIfLong);
      } else {
        TRI_ASSERT(false);
      }
    }
    buffer->appendChar('}');
    return;
  }

  if (type == NODE_TYPE_REFERENCE || type == NODE_TYPE_VARIABLE) {
    // not used by V8
    auto variable = static_cast<Variable*>(getData());
    TRI_ASSERT(variable != nullptr);
    // we're intentionally not using the variable name as it is not necessarily
    // unique within a query (hey COLLECT, I am looking at you!)
    buffer->appendChar('$');
    buffer->appendInteger(variable->id);
    return;
  }

  if (type == NODE_TYPE_INDEXED_ACCESS) {
    // not used by V8
    auto member = getMember(0);
    auto index = getMember(1);
    member->stringify(buffer, verbose, failIfLong);
    buffer->appendChar('[');
    index->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(']');
    return;
  }

  if (type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    // not used by V8
    auto member = getMember(0);
    member->stringify(buffer, verbose, failIfLong);
    buffer->appendChar('.');
    buffer->appendText(getStringValue(), getStringLength());
    return;
  }

  if (type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS) {
    // not used by V8
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar('.');
    getMember(1)->stringify(buffer, verbose, failIfLong);
    return;
  }

  if (type == NODE_TYPE_PARAMETER || type == NODE_TYPE_PARAMETER_DATASOURCE) {
    // not used by V8
    buffer->appendChar('@');
    buffer->appendText(getStringValue(), getStringLength());
    return;
  }

  if (type == NODE_TYPE_FCALL) {
    // not used by V8
    auto func = static_cast<Function*>(getData());
    buffer->appendText(func->name);
    buffer->appendChar('(');
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(')');
    return;
  }

  if (type == NODE_TYPE_ARRAY_LIMIT) {
    // not used by V8
    buffer->appendText(TRI_CHAR_LENGTH_PAIR("_LIMIT("));
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(',');
    getMember(1)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(')');
    return;
  }

  if (type == NODE_TYPE_EXPANSION) {
    // not used by V8
    buffer->appendText(TRI_CHAR_LENGTH_PAIR("_EXPANSION("));
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(',');
    getMember(1)->stringify(buffer, verbose, failIfLong);
    // filter
    buffer->appendChar(',');

    auto filterNode = getMember(2);
    if (filterNode != nullptr && filterNode->type != NODE_TYPE_NOP) {
      buffer->appendText(TRI_CHAR_LENGTH_PAIR(" FILTER "));
      filterNode->getMember(0)->stringify(buffer, verbose, failIfLong);
    }
    auto limitNode = getMember(3);
    if (limitNode != nullptr && limitNode->type != NODE_TYPE_NOP) {
      buffer->appendText(TRI_CHAR_LENGTH_PAIR(" LIMIT "));
      limitNode->getMember(0)->stringify(buffer, verbose, failIfLong);
      buffer->appendChar(',');
      limitNode->getMember(1)->stringify(buffer, verbose, failIfLong);
    }
    auto returnNode = getMember(4);
    if (returnNode != nullptr && returnNode->type != NODE_TYPE_NOP) {
      buffer->appendText(TRI_CHAR_LENGTH_PAIR(" RETURN "));
      returnNode->stringify(buffer, verbose, failIfLong);
    }

    buffer->appendChar(')');
    return;
  }

  if (type == NODE_TYPE_ITERATOR) {
    // not used by V8
    buffer->appendText(TRI_CHAR_LENGTH_PAIR("_ITERATOR("));
    getMember(1)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(',');
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(')');
    return;
  }

  if (type == NODE_TYPE_OPERATOR_UNARY_NOT || type == NODE_TYPE_OPERATOR_UNARY_PLUS ||
      type == NODE_TYPE_OPERATOR_UNARY_MINUS) {
    // not used by V8
    TRI_ASSERT(numMembers() == 1);
    auto it = Operators.find(static_cast<int>(type));
    TRI_ASSERT(it != Operators.end());
    buffer->appendChar(' ');
    buffer->appendText((*it).second);

    getMember(0)->stringify(buffer, verbose, failIfLong);
    return;
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_AND || type == NODE_TYPE_OPERATOR_BINARY_OR ||
      type == NODE_TYPE_OPERATOR_BINARY_PLUS || type == NODE_TYPE_OPERATOR_BINARY_MINUS ||
      type == NODE_TYPE_OPERATOR_BINARY_TIMES ||
      type == NODE_TYPE_OPERATOR_BINARY_DIV || type == NODE_TYPE_OPERATOR_BINARY_MOD ||
      type == NODE_TYPE_OPERATOR_BINARY_EQ || type == NODE_TYPE_OPERATOR_BINARY_NE ||
      type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
      type == NODE_TYPE_OPERATOR_BINARY_GT || type == NODE_TYPE_OPERATOR_BINARY_GE ||
      type == NODE_TYPE_OPERATOR_BINARY_IN || type == NODE_TYPE_OPERATOR_BINARY_NIN) {
    // not used by V8
    TRI_ASSERT(numMembers() == 2);
    auto it = Operators.find(type);
    TRI_ASSERT(it != Operators.end());

    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(' ');
    buffer->appendText((*it).second);
    buffer->appendChar(' ');
    getMember(1)->stringify(buffer, verbose, failIfLong);
    return;
  }

  if (type == NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_LE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GT ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_GE || type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
      type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
    // not used by V8
    TRI_ASSERT(numMembers() == 3);
    auto it = Operators.find(type);
    TRI_ASSERT(it != Operators.end());

    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar(' ');
    buffer->appendText(Quantifier::Stringify(getMember(2)->getIntValue(true)));
    buffer->appendChar(' ');
    buffer->appendText((*it).second);
    buffer->appendChar(' ');
    getMember(1)->stringify(buffer, verbose, failIfLong);
    return;
  }

  if (type == NODE_TYPE_OPERATOR_TERNARY) {
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendChar('?');
    if (numMembers() == 3) {
      getMember(1)->stringify(buffer, verbose, failIfLong);
      buffer->appendChar(':');
      getMember(2)->stringify(buffer, verbose, failIfLong);
    } else {
      buffer->appendChar(':');
      getMember(1)->stringify(buffer, verbose, failIfLong);
    }
    return;
  }

  if (type == NODE_TYPE_OPERATOR_NARY_AND || type == NODE_TYPE_OPERATOR_NARY_OR) {
    // not used by V8
    size_t const n = numMembers();
    for (size_t i = 0; i < n; ++i) {
      if (i > 0) {
        if (type == NODE_TYPE_OPERATOR_NARY_AND) {
          buffer->appendText(" AND ");
        } else {
          buffer->appendText(" OR ");
        }
      }
      getMember(i)->stringify(buffer, verbose, failIfLong);
    }
    return;
  }

  if (type == NODE_TYPE_RANGE) {
    // not used by V8
    TRI_ASSERT(numMembers() == 2);
    getMember(0)->stringify(buffer, verbose, failIfLong);
    buffer->appendText(TRI_CHAR_LENGTH_PAIR(".."));
    getMember(1)->stringify(buffer, verbose, failIfLong);
    return;
  }

  std::string message("stringification not supported for node type ");
  message.append(getTypeString());

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, message);
}

/// note that this may throw and that the caller is responsible for
/// catching the error
std::string AstNode::toString() const {
  arangodb::basics::StringBuffer buffer(false);
  stringify(&buffer, false, false);
  return std::string(buffer.data(), buffer.length());
}

/// @brief locate a variable including the direct path vector leading to it.
void AstNode::findVariableAccess(std::vector<AstNode const*>& currentPath,
                                 std::vector<std::vector<AstNode const*>>& paths,
                                 Variable const* findme) const {
  currentPath.push_back(this);
  switch (type) {
    case NODE_TYPE_VARIABLE:
    case NODE_TYPE_REFERENCE: {
      auto variable = static_cast<Variable*>(getData());
      TRI_ASSERT(variable != nullptr);
      if (variable->id == findme->id) {
        paths.emplace_back(currentPath);
      }
    } break;

    case NODE_TYPE_OPERATOR_NARY_AND:
    case NODE_TYPE_OPERATOR_NARY_OR:
    case NODE_TYPE_OBJECT:  // yes, keys can't be vars, but...
    case NODE_TYPE_ARRAY: {
      size_t const n = numMembers();
      for (size_t i = 0; (i < n); ++i) {
        AstNode* member = getMember(i);
        if (member != nullptr) {
          member->findVariableAccess(currentPath, paths, findme);
        }
      }
    } break;

    // One subnode:
    case NODE_TYPE_FCALL:
    case NODE_TYPE_ATTRIBUTE_ACCESS:
    case NODE_TYPE_OPERATOR_UNARY_PLUS:
    case NODE_TYPE_OPERATOR_UNARY_MINUS:
    case NODE_TYPE_OPERATOR_UNARY_NOT:
      getMember(0)->findVariableAccess(currentPath, paths, findme);
      break;

    // two subnodes to inspect:
    case NODE_TYPE_RANGE:
    case NODE_TYPE_ITERATOR:
    case NODE_TYPE_ARRAY_LIMIT:
    case NODE_TYPE_INDEXED_ACCESS:
    case NODE_TYPE_BOUND_ATTRIBUTE_ACCESS:
    case NODE_TYPE_OPERATOR_BINARY_AND:
    case NODE_TYPE_OPERATOR_BINARY_OR:
    case NODE_TYPE_OPERATOR_BINARY_PLUS:
    case NODE_TYPE_OPERATOR_BINARY_MINUS:
    case NODE_TYPE_OPERATOR_BINARY_TIMES:
    case NODE_TYPE_OPERATOR_BINARY_DIV:
    case NODE_TYPE_OPERATOR_BINARY_MOD:
    case NODE_TYPE_OPERATOR_BINARY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_NE:
    case NODE_TYPE_OPERATOR_BINARY_LT:
    case NODE_TYPE_OPERATOR_BINARY_LE:
    case NODE_TYPE_OPERATOR_BINARY_GT:
    case NODE_TYPE_OPERATOR_BINARY_GE:
    case NODE_TYPE_OPERATOR_BINARY_IN:
    case NODE_TYPE_OPERATOR_BINARY_NIN:
      getMember(0)->findVariableAccess(currentPath, paths, findme);
      getMember(1)->findVariableAccess(currentPath, paths, findme);
      break;

    case NODE_TYPE_EXPANSION: {
      getMember(0)->findVariableAccess(currentPath, paths, findme);
      getMember(1)->findVariableAccess(currentPath, paths, findme);
      auto filterNode = getMember(2);
      if (filterNode != nullptr) {
        filterNode->findVariableAccess(currentPath, paths, findme);
      }
      auto limitNode = getMember(3);
      if (limitNode != nullptr) {
        limitNode->findVariableAccess(currentPath, paths, findme);
      }
      auto returnNode = getMember(4);
      if (returnNode != nullptr) {
        returnNode->findVariableAccess(currentPath, paths, findme);
      }
    } break;
    // no sub nodes, not a variable:
    case NODE_TYPE_OPERATOR_TERNARY:
    case NODE_TYPE_SUBQUERY:
    case NODE_TYPE_VALUE:
    case NODE_TYPE_ROOT:
    case NODE_TYPE_FOR:
    case NODE_TYPE_LET:
    case NODE_TYPE_FILTER:
    case NODE_TYPE_RETURN:
    case NODE_TYPE_REMOVE:
    case NODE_TYPE_INSERT:
    case NODE_TYPE_UPDATE:
    case NODE_TYPE_REPLACE:
    case NODE_TYPE_COLLECT:
    case NODE_TYPE_SORT:
    case NODE_TYPE_SORT_ELEMENT:
    case NODE_TYPE_LIMIT:
    case NODE_TYPE_ASSIGN:
    case NODE_TYPE_OBJECT_ELEMENT:
    case NODE_TYPE_COLLECTION:
    case NODE_TYPE_VIEW:
    case NODE_TYPE_PARAMETER:
    case NODE_TYPE_PARAMETER_DATASOURCE:
    case NODE_TYPE_FCALL_USER:
    case NODE_TYPE_NOP:
    case NODE_TYPE_COLLECT_COUNT:
    case NODE_TYPE_AGGREGATIONS:
    case NODE_TYPE_CALCULATED_OBJECT_ELEMENT:
    case NODE_TYPE_UPSERT:
    case NODE_TYPE_EXAMPLE:
    case NODE_TYPE_PASSTHRU:
    case NODE_TYPE_DISTINCT:
    case NODE_TYPE_TRAVERSAL:
    case NODE_TYPE_SHORTEST_PATH:
    case NODE_TYPE_K_SHORTEST_PATHS:
    case NODE_TYPE_COLLECTION_LIST:
    case NODE_TYPE_DIRECTION:
    case NODE_TYPE_WITH:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_IN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN:
    case NODE_TYPE_QUANTIFIER:
    case NODE_TYPE_FOR_VIEW:
    case NODE_TYPE_READ_ALL:
      break;
  }

  currentPath.pop_back();
}

AstNode const* AstNode::findReference(AstNode const* findme) const {
  if (this == findme) {
    return this;
  }

  const AstNode* ret = nullptr;
  switch (type) {
    case NODE_TYPE_OBJECT:  // yes, keys can't be vars, but...
    case NODE_TYPE_ARRAY: {
      size_t const n = numMembers();
      for (size_t i = 0; i < n; ++i) {
        AstNode* member = getMember(i);
        if (member == findme) {
          return this;
        }
        if (member != nullptr) {
          ret = member->findReference(findme);
          if (ret != nullptr) {
            return ret;
          }
        }
      }
    } break;

    // One subnode:
    case NODE_TYPE_FCALL:
    case NODE_TYPE_ATTRIBUTE_ACCESS:
    case NODE_TYPE_OPERATOR_UNARY_PLUS:
    case NODE_TYPE_OPERATOR_UNARY_MINUS:
    case NODE_TYPE_OPERATOR_UNARY_NOT:
      if (getMember(0) == findme) {
        return this;
      }
      return getMember(0)->findReference(findme);

    // two subnodes to inspect:
    case NODE_TYPE_RANGE:
    case NODE_TYPE_ITERATOR:
    case NODE_TYPE_ARRAY_LIMIT:
    case NODE_TYPE_INDEXED_ACCESS:
    case NODE_TYPE_BOUND_ATTRIBUTE_ACCESS:
    case NODE_TYPE_OPERATOR_BINARY_AND:
    case NODE_TYPE_OPERATOR_BINARY_OR:
    case NODE_TYPE_OPERATOR_BINARY_PLUS:
    case NODE_TYPE_OPERATOR_BINARY_MINUS:
    case NODE_TYPE_OPERATOR_BINARY_TIMES:
    case NODE_TYPE_OPERATOR_BINARY_DIV:
    case NODE_TYPE_OPERATOR_BINARY_MOD:
    case NODE_TYPE_OPERATOR_BINARY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_NE:
    case NODE_TYPE_OPERATOR_BINARY_LT:
    case NODE_TYPE_OPERATOR_BINARY_LE:
    case NODE_TYPE_OPERATOR_BINARY_GT:
    case NODE_TYPE_OPERATOR_BINARY_GE:
    case NODE_TYPE_OPERATOR_BINARY_IN:
    case NODE_TYPE_OPERATOR_BINARY_NIN:
      if (getMember(0) == findme) {
        return this;
      }
      ret = getMember(0)->findReference(findme);
      if (ret != nullptr) {
        return ret;
      }
      if (getMember(1) == findme) {
        return this;
      }
      ret = getMember(1)->findReference(findme);
      break;

    case NODE_TYPE_EXPANSION: {
      if (getMember(0) == findme) {
        return this;
      }
      ret = getMember(0)->findReference(findme);
      if (ret != nullptr) {
        return ret;
      }
      if (getMember(1) == findme) {
        return this;
      }
      ret = getMember(1)->findReference(findme);
      if (ret != nullptr) {
        return ret;
      }
      auto filterNode = getMember(2);
      if (filterNode != nullptr) {
        if (filterNode->getMember(0) == findme) {
          return this;
        }

        ret = filterNode->getMember(0)->findReference(findme);
        if (ret != nullptr) {
          return ret;
        }
      }
      auto limitNode = getMember(3);
      if (limitNode != nullptr) {
        if (limitNode->getMember(0) == findme) {
          return this;
        }
        ret = limitNode->getMember(0)->findReference(findme);
        if (ret != nullptr) {
          return ret;
        }
        ret = limitNode->getMember(1)->findReference(findme);
        if (ret != nullptr) {
          return ret;
        }
      }
      auto returnNode = getMember(4);
      if (returnNode != nullptr) {
        if (returnNode->getMember(0) == findme) {
          return this;
        }
        ret = returnNode->getMember(0)->findReference(findme);
      }
    } break;

    // no subnodes here:
    case NODE_TYPE_OPERATOR_TERNARY:
    case NODE_TYPE_SUBQUERY:
    case NODE_TYPE_VALUE:
    case NODE_TYPE_ROOT:
    case NODE_TYPE_FOR:
    case NODE_TYPE_LET:
    case NODE_TYPE_FILTER:
    case NODE_TYPE_RETURN:
    case NODE_TYPE_REMOVE:
    case NODE_TYPE_INSERT:
    case NODE_TYPE_UPDATE:
    case NODE_TYPE_REPLACE:
    case NODE_TYPE_COLLECT:
    case NODE_TYPE_SORT:
    case NODE_TYPE_SORT_ELEMENT:
    case NODE_TYPE_LIMIT:
    case NODE_TYPE_ASSIGN:
    case NODE_TYPE_VARIABLE:
    case NODE_TYPE_REFERENCE:
    case NODE_TYPE_OBJECT_ELEMENT:
    case NODE_TYPE_COLLECTION:
    case NODE_TYPE_VIEW:
    case NODE_TYPE_PARAMETER:
    case NODE_TYPE_PARAMETER_DATASOURCE:
    case NODE_TYPE_FCALL_USER:
    case NODE_TYPE_NOP:
    case NODE_TYPE_COLLECT_COUNT:
    case NODE_TYPE_AGGREGATIONS:
    case NODE_TYPE_CALCULATED_OBJECT_ELEMENT:
    case NODE_TYPE_UPSERT:
    case NODE_TYPE_EXAMPLE:
    case NODE_TYPE_PASSTHRU:
    case NODE_TYPE_DISTINCT:
    case NODE_TYPE_TRAVERSAL:
    case NODE_TYPE_SHORTEST_PATH:
    case NODE_TYPE_K_SHORTEST_PATHS:
    case NODE_TYPE_COLLECTION_LIST:
    case NODE_TYPE_DIRECTION:
    case NODE_TYPE_OPERATOR_NARY_AND:
    case NODE_TYPE_OPERATOR_NARY_OR:
    case NODE_TYPE_WITH:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_LE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GT:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_GE:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_IN:
    case NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN:
    case NODE_TYPE_QUANTIFIER:
    case NODE_TYPE_FOR_VIEW:
    case NODE_TYPE_READ_ALL:
      break;
  }
  return ret;
}

/// @brief stringify the value of a node into a string buffer
/// this creates an equivalent to what JSON.stringify() would do
/// this method is used when generated JavaScript code for the node!
void AstNode::appendValue(arangodb::basics::StringBuffer* buffer) const {
  TRI_ASSERT(type == NODE_TYPE_VALUE);

  switch (value.type) {
    case VALUE_TYPE_BOOL: {
      if (value.value._bool) {
        buffer->appendText(TRI_CHAR_LENGTH_PAIR("true"));
      } else {
        buffer->appendText(TRI_CHAR_LENGTH_PAIR("false"));
      }
      break;
    }

    case VALUE_TYPE_INT: {
      buffer->appendInteger(value.value._int);
      break;
    }

    case VALUE_TYPE_DOUBLE: {
      double const v = value.value._double;
      if (std::isnan(v) || !std::isfinite(v) || v == HUGE_VAL || v == -HUGE_VAL) {
        buffer->appendText(TRI_CHAR_LENGTH_PAIR("null"));
      } else {
        buffer->appendDecimal(value.value._double);
      }
      break;
    }

    case VALUE_TYPE_STRING: {
      buffer->appendJsonEncoded(value.value._string, value.length);
      break;
    }

    case VALUE_TYPE_NULL:
    default: {
      buffer->appendText(TRI_CHAR_LENGTH_PAIR("null"));
      break;
    }
  }
}

void AstNode::markFinalized(AstNode* subtreeRoot) {
  if ((nullptr == subtreeRoot) || subtreeRoot->hasFlag(AstNodeFlagType::FLAG_FINALIZED)) {
    return;
  }

  subtreeRoot->setFlag(AstNodeFlagType::FLAG_FINALIZED);
  size_t const n = subtreeRoot->numMembers();
  for (size_t i = 0; i < n; ++i) {
    markFinalized(subtreeRoot->getMemberUnchecked(i));
  }
}

template <typename... Args>
std::underlying_type<AstNodeFlagType>::type AstNode::makeFlags(AstNodeFlagType flag,
                                                               Args... args) {
  return static_cast<std::underlying_type<AstNodeFlagType>::type>(flag) +
         makeFlags(args...);
}

std::underlying_type<AstNodeFlagType>::type AstNode::makeFlags() {
  return static_cast<std::underlying_type<AstNodeFlagType>::type>(0);
}

bool AstNode::hasFlag(AstNodeFlagType flag) const {
  return ((flags & static_cast<decltype(flags)>(flag)) != 0);
}

void AstNode::clearFlags() { 
  // clear all flags but this one
  flags &= AstNodeFlagType::FLAG_INTERNAL_CONST; 
}

void AstNode::setFlag(AstNodeFlagType flag) const { flags |= flag; }

void AstNode::setFlag(AstNodeFlagType typeFlag, AstNodeFlagType valueFlag) const {
  flags |= (typeFlag | valueFlag);
}

void AstNode::removeFlag(AstNodeFlagType flag) const { flags &= ~flag; }

bool AstNode::isSorted() const {
  return ((flags & (DETERMINED_SORTED | VALUE_SORTED)) == (DETERMINED_SORTED | VALUE_SORTED));
}

bool AstNode::isNullValue() const {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_NULL);
}

bool AstNode::isIntValue() const {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_INT);
}

bool AstNode::isDoubleValue() const {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_DOUBLE);
}

bool AstNode::isNumericValue() const {
  return (type == NODE_TYPE_VALUE &&
          (value.type == VALUE_TYPE_INT || value.type == VALUE_TYPE_DOUBLE));
}

bool AstNode::isBoolValue() const {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_BOOL);
}

bool AstNode::isStringValue() const {
  return (type == NODE_TYPE_VALUE && value.type == VALUE_TYPE_STRING);
}

bool AstNode::isArray() const { return (type == NODE_TYPE_ARRAY); }

bool AstNode::isObject() const { return (type == NODE_TYPE_OBJECT); }

AstNode const* AstNode::getAttributeAccessForVariable(bool allowIndexedAccess) const {
  if (type != NODE_TYPE_ATTRIBUTE_ACCESS && type != NODE_TYPE_EXPANSION &&
      !(allowIndexedAccess && type == NODE_TYPE_INDEXED_ACCESS)) {
    return nullptr;
  }

  auto node = this;

  while (node->type == NODE_TYPE_ATTRIBUTE_ACCESS ||
         (allowIndexedAccess && node->type == NODE_TYPE_INDEXED_ACCESS) ||
         node->type == NODE_TYPE_EXPANSION) {
    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS || node->type == NODE_TYPE_INDEXED_ACCESS) {
      node = node->getMember(0);
    } else {
      // expansion, i.e. [*]
      TRI_ASSERT(node->type == NODE_TYPE_EXPANSION);
      TRI_ASSERT(node->numMembers() >= 2);

      if (node->getMember(1)->type != NODE_TYPE_REFERENCE) {
        if (node->getMember(1)->getAttributeAccessForVariable(allowIndexedAccess) == nullptr) {
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

void AstNode::sortMembers(std::function<bool(AstNode const*, AstNode const*)> const& func) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  std::sort(members.begin(), members.end(), func);
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
  TRI_ASSERT(type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
             type == NODE_TYPE_OPERATOR_BINARY_EQ);
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  value.value._bool = v;
}

bool AstNode::getExcludesNull() const noexcept {
  TRI_ASSERT(type == NODE_TYPE_OPERATOR_BINARY_LT || type == NODE_TYPE_OPERATOR_BINARY_LE ||
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

void* AstNode::getData() const { return value.value._data; }

void AstNode::setData(void* v) {
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_FINALIZED));
  TRI_ASSERT(!hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));

  freeComputedValue();
  value.value._data = v;
}

void AstNode::setData(void const* v) {
  setData(const_cast<void*>(v));
}

void AstNode::freeComputedValue() {
  if (_computedValue != nullptr && !hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST)) {
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
std::ostream& operator<<(std::ostream& stream, arangodb::aql::AstNode const* node) {
  if (node != nullptr) {
    stream << arangodb::aql::AstNode::toString(node);
  }
  return stream;
}

/// @brief append the AstNode to an output stream
std::ostream& operator<<(std::ostream& stream, arangodb::aql::AstNode const& node) {
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

template <bool useUtf8>
bool AstNodeValueLess<useUtf8>::operator()(AstNode const* lhs, AstNode const* rhs) const {
  return CompareAstNodes(lhs, rhs, useUtf8) < 0;
}
template struct ::arangodb::aql::AstNodeValueLess<true>;
template struct ::arangodb::aql::AstNodeValueLess<false>;

size_t AstNodeValueHash::operator()(AstNode const* value) const noexcept {
  return static_cast<size_t>(value->hashValue(0x12345678));
}

bool AstNodeValueEqual::operator()(AstNode const* lhs, AstNode const* rhs) const {
  return CompareAstNodes(lhs, rhs, false) == 0;
}
