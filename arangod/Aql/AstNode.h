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

#pragma once

#include "Basics/ScopeGuard.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arangodb {
namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack
namespace basics {
struct AttributeName;
}  // namespace basics

namespace aql {
class Ast;
struct Variable;

/// @brief type for node flags
using AstNodeFlagsType = uint32_t;

/// @brief different flags for nodes
/// the flags are used to prevent repeated calculations of node properties
/// (e.g. is the node value constant, sorted etc.)
enum AstNodeFlagType : AstNodeFlagsType {
  DETERMINED_SORTED =
      0x0000001,  // node is a list and its members are sorted asc.
  DETERMINED_CONSTANT = 0x0000002,  // node value is constant (i.e. not dynamic)
  DETERMINED_SIMPLE =
      0x0000004,  // node value is simple (i.e. for use in a simple expression)
  DETERMINED_NONDETERMINISTIC = 0x0000010,  // node produces non-deterministic
                                            // result (e.g. function call nodes)
  DETERMINED_RUNONDBSERVER =
      0x0000020,  // node can run on the DB server in a cluster setup
  DETERMINED_CHECKUNIQUENESS =
      0x0000040,              // object's keys must be checked for uniqueness
  DETERMINED_V8 = 0x0000080,  // node will use V8 internally

  VALUE_SORTED = 0x0000100,    // node is a list and its members are sorted asc.
  VALUE_CONSTANT = 0x0000200,  // node value is constant (i.e. not dynamic)
  VALUE_SIMPLE =
      0x0000400,  // node value is simple (i.e. for use in a simple expression)
  VALUE_NONDETERMINISTIC = 0x0001000,  // node produces non-deterministic result
                                       // (e.g. function call nodes)
  VALUE_RUNONDBSERVER =
      0x0002000,  // node can run on the DB server in a cluster setup
  VALUE_CHECKUNIQUENESS =
      0x0004000,         // object's keys must be checked for uniqueness
  VALUE_V8 = 0x0008000,  // node will use V8 internally
  FLAG_KEEP_VARIABLENAME =
      0x0010000,  // node is a reference to a variable name, not the variable
                  // value (used in KEEP nodes)
  FLAG_BIND_PARAMETER = 0x0020000,  // node was created from a bind parameter
  FLAG_FINALIZED =
      0x0040000,  // node has been finalized and should not be modified; only
                  // set and checked in maintainer mode
  FLAG_SUBQUERY_REFERENCE = 0x0080000,  // node references a subquery

  FLAG_INTERNAL_CONST = 0x0100000,  // internal, constant node

  FLAG_BOOLEAN_EXPANSION = 0x0200000,  // make expansion result boolean

  FLAG_READ_OWN_WRITES =
      0x0400000,  // reads own writes (only needed for UPSERT FOR nodes)
};

/// @brief enumeration of AST node value types
/// note: these types must be declared in asc. sort order
enum AstNodeValueType : uint8_t {
  VALUE_TYPE_NULL = 0,
  VALUE_TYPE_BOOL = 1,
  VALUE_TYPE_INT = 2,
  VALUE_TYPE_DOUBLE = 3,
  VALUE_TYPE_STRING = 4
};

static_assert(VALUE_TYPE_NULL < VALUE_TYPE_BOOL,
              "incorrect ast node value types");
static_assert(VALUE_TYPE_BOOL < VALUE_TYPE_INT,
              "incorrect ast node value types");
static_assert(VALUE_TYPE_INT < VALUE_TYPE_DOUBLE,
              "incorrect ast node value types");
static_assert(VALUE_TYPE_DOUBLE < VALUE_TYPE_STRING,
              "incorrect ast node value types");

/// @brief AST node value
struct AstNodeValue {
  union Value {
    int64_t _int;
    double _double;
    bool _bool;
    char const* _string;
    void* _data;

    // constructors for union values
    explicit Value(int64_t value);
    explicit Value(double value);
    explicit Value(bool value);
    explicit Value(char const* value);
  };

  Value value;
  uint32_t length;  // only used for string values
  AstNodeValueType type;

  AstNodeValue();
  explicit AstNodeValue(int64_t value);
  explicit AstNodeValue(double value);
  explicit AstNodeValue(bool value);
  explicit AstNodeValue(char const* value, uint32_t length);
};

/// @brief enumeration of AST node types
enum AstNodeType : uint32_t {
  NODE_TYPE_ROOT = 0,
  NODE_TYPE_FOR = 1,
  NODE_TYPE_LET = 2,
  NODE_TYPE_FILTER = 3,
  NODE_TYPE_RETURN = 4,
  NODE_TYPE_REMOVE = 5,
  NODE_TYPE_INSERT = 6,
  NODE_TYPE_UPDATE = 7,
  NODE_TYPE_REPLACE = 8,
  NODE_TYPE_COLLECT = 9,
  NODE_TYPE_SORT = 10,
  NODE_TYPE_SORT_ELEMENT = 11,
  NODE_TYPE_LIMIT = 12,
  NODE_TYPE_VARIABLE = 13,
  NODE_TYPE_ASSIGN = 14,
  NODE_TYPE_OPERATOR_UNARY_PLUS = 15,
  NODE_TYPE_OPERATOR_UNARY_MINUS = 16,
  NODE_TYPE_OPERATOR_UNARY_NOT = 17,
  NODE_TYPE_OPERATOR_BINARY_AND = 18,
  NODE_TYPE_OPERATOR_BINARY_OR = 19,
  NODE_TYPE_OPERATOR_BINARY_PLUS = 20,
  NODE_TYPE_OPERATOR_BINARY_MINUS = 21,
  NODE_TYPE_OPERATOR_BINARY_TIMES = 22,
  NODE_TYPE_OPERATOR_BINARY_DIV = 23,
  NODE_TYPE_OPERATOR_BINARY_MOD = 24,
  NODE_TYPE_OPERATOR_BINARY_EQ = 25,
  NODE_TYPE_OPERATOR_BINARY_NE = 26,
  NODE_TYPE_OPERATOR_BINARY_LT = 27,
  NODE_TYPE_OPERATOR_BINARY_LE = 28,
  NODE_TYPE_OPERATOR_BINARY_GT = 29,
  NODE_TYPE_OPERATOR_BINARY_GE = 30,
  NODE_TYPE_OPERATOR_BINARY_IN = 31,
  NODE_TYPE_OPERATOR_BINARY_NIN = 32,
  NODE_TYPE_OPERATOR_TERNARY = 33,
  NODE_TYPE_SUBQUERY = 34,
  NODE_TYPE_ATTRIBUTE_ACCESS = 35,
  NODE_TYPE_BOUND_ATTRIBUTE_ACCESS = 36,
  NODE_TYPE_INDEXED_ACCESS = 37,
  NODE_TYPE_EXPANSION = 38,
  NODE_TYPE_ITERATOR = 39,
  NODE_TYPE_VALUE = 40,
  NODE_TYPE_ARRAY = 41,
  NODE_TYPE_OBJECT = 42,
  NODE_TYPE_OBJECT_ELEMENT = 43,
  NODE_TYPE_COLLECTION = 44,
  NODE_TYPE_REFERENCE = 45,
  NODE_TYPE_PARAMETER = 46,
  NODE_TYPE_FCALL = 47,
  NODE_TYPE_FCALL_USER = 48,
  NODE_TYPE_RANGE = 49,
  NODE_TYPE_NOP = 50,
  NODE_TYPE_CALCULATED_OBJECT_ELEMENT = 53,
  NODE_TYPE_UPSERT = 54,
  NODE_TYPE_EXAMPLE = 55,
  NODE_TYPE_PASSTHRU = 56,
  NODE_TYPE_ARRAY_LIMIT = 57,
  NODE_TYPE_DISTINCT = 58,
  NODE_TYPE_TRAVERSAL = 59,
  NODE_TYPE_COLLECTION_LIST = 60,
  NODE_TYPE_DIRECTION = 61,
  NODE_TYPE_OPERATOR_NARY_AND = 62,
  NODE_TYPE_OPERATOR_NARY_OR = 63,
  NODE_TYPE_AGGREGATIONS = 64,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_EQ = 65,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_NE = 66,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_LT = 67,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_LE = 68,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_GT = 69,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_GE = 70,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_IN = 71,
  NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN = 72,
  NODE_TYPE_QUANTIFIER = 73,
  NODE_TYPE_WITH = 74,
  NODE_TYPE_SHORTEST_PATH = 75,
  NODE_TYPE_ENUMERATE_PATHS = 76,
  NODE_TYPE_VIEW = 77,
  NODE_TYPE_PARAMETER_DATASOURCE = 78,
  NODE_TYPE_FOR_VIEW = 79,
  NODE_TYPE_WINDOW = 80,
  NODE_TYPE_ARRAY_FILTER = 81,
};

static_assert(NODE_TYPE_VALUE < NODE_TYPE_ARRAY, "incorrect node types order");
static_assert(NODE_TYPE_ARRAY < NODE_TYPE_OBJECT, "incorrect node types order");

/// @brief the node
struct AstNode {
  friend class Ast;

  /// @brief a simple tag that marks the AstNode as a constant node
  /// that will never change after being created
  struct InternalNode {};

  /// @brief array values with at least this number of members that
  /// are in IN or NOT IN lookups will be sorted, so that we can use
  /// a binary sort to do lookups
  static constexpr size_t kSortNumberThreshold = 8;

  /// @brief create the node
  explicit AstNode(AstNodeType);

  explicit AstNode(AstNodeType, InternalNode);

  /// @brief create a node, with defining a value
  explicit AstNode(AstNodeValue const& value);

  explicit AstNode(AstNodeValue const& value, InternalNode);

  /// @brief create the node from VPack
  explicit AstNode(Ast*, arangodb::velocypack::Slice slice);

  /// @brief destroy the node
  ~AstNode();

  /// @brief return the string value of a node, as an std::string
  std::string getString() const;

  /// @brief return the string value of a node
  std::string_view getStringView() const noexcept;

  /// @brief test if all members of a node are equality comparisons
  bool isOnlyEqualityMatch() const;

  /// @brief computes a hash value for a value node
  uint64_t hashValue(uint64_t) const noexcept;

/// @brief dump the node (for debugging purposes)
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  std::ostream& toStream(std::ostream& os, int indent) const;
  void dump(int indent) const;
#endif

  /// @brief compute the value for a constant value node
  /// the value is owned by the node and must not be freed by the caller
  /// the Builder object can be passed in as an optimization
  arangodb::velocypack::Slice computeValue(
      arangodb::velocypack::Builder* = nullptr) const;

  uint8_t const* computedValue() const noexcept { return _computedValue; }

  /// @brief sort the members of an (array) node
  /// this will also set the FLAG_SORTED flag for the node
  void sort();

  /// @brief return the type name of a node
  std::string_view getTypeString() const;

  /// @brief return the type name for a type
  static std::optional<std::string_view> getTypeStringForType(AstNodeType type);

  /// @brief return the value type name of a node
  std::string_view getValueTypeString() const;

  /// @brief stringify the AstNode
  static std::string toString(AstNode const*);

  /// @brief checks whether we know a type of this kind; throws exception if not
  static void validateType(int type);

  /// @brief checks whether we know a value type of this kind;
  /// throws exception if not
  static void validateValueType(int type);

  /// @brief fetch a node's type from VPack
  static AstNodeType getNodeTypeFromVPack(arangodb::velocypack::Slice slice);

  void setConstantFlags() noexcept;

  /// @brief function to check if this node can be represented as VelocyPack.
  bool valueHasVelocyPackRepresentation() const;

  /// @brief build a VelocyPack representation of the node value
  ///        Can throw Out of Memory Error
  void toVelocyPackValue(arangodb::velocypack::Builder& builder) const;

  /// @brief Create a VelocyPack representation of the node
  void toVelocyPack(arangodb::velocypack::Builder& builder, bool verbose) const;

  /// @brief convert the node's value to a boolean value
  /// this may create a new node or return the node itself if it is already a
  /// boolean value node
  AstNode const* castToBool(Ast*) const;

  /// @brief convert the node's value to a number value
  /// this may create a new node or return the node itself if it is already a
  /// numeric value node
  AstNode const* castToNumber(Ast*) const;

  /// @brief check a flag for the node
  bool hasFlag(AstNodeFlagType flag) const noexcept;

  /// @brief reset flags in case a node is changed drastically
  void clearFlags() noexcept;

  /// @brief recursively clear flags
  void clearFlagsRecursive() noexcept;

  /// @brief set a flag for the node
  void setFlag(AstNodeFlagType flag) const noexcept;

  /// @brief set two flags for the node
  void setFlag(AstNodeFlagType typeFlag,
               AstNodeFlagType valueFlag) const noexcept;

  /// @brief remove a flag for the node
  void removeFlag(AstNodeFlagType flag) const noexcept;

  /// @brief whether or not the node value is trueish
  bool isTrue() const;

  /// @brief whether or not the node value is falsey
  bool isFalse() const;

  /// @brief whether or not the members of a list node are sorted
  bool isSorted() const noexcept;

  /// @brief whether or not a value node is NULL
  bool isNullValue() const noexcept;

  /// @brief whether or not a value node is an integer
  bool isIntValue() const noexcept;

  /// @brief whether or not a value node is a dobule
  bool isDoubleValue() const noexcept;

  /// @brief whether or not a value node is of numeric type
  bool isNumericValue() const noexcept;

  /// @brief whether or not a value node is of bool type
  bool isBoolValue() const noexcept;

  /// @brief whether or not a value node is of string type
  bool isStringValue() const noexcept;

  /// @brief whether or not a value node is of list type
  bool isArray() const noexcept;

  /// @brief whether or not a value node is of array type
  bool isObject() const noexcept;

  /// @brief whether or not a value node is of type attribute access that
  /// refers to a variable reference
  AstNode const* getAttributeAccessForVariable(bool allowIndexedAccess) const;

  /// @brief whether or not a value node is of type attribute access that
  /// refers to any variable reference
  bool isAttributeAccessForVariable() const;

  /// @brief whether or not a value node is of type attribute access that
  /// refers to the specified variable reference
  bool isAttributeAccessForVariable(Variable const* variable,
                                    bool allowIndexedAccess) const;

  /// @brief whether or not a value node is of type attribute access that
  /// refers to any variable reference
  /// returns true if yes, and then also returns variable reference and array
  /// of attribute names in the parameter passed by reference
  bool isAttributeAccessForVariable(
      std::pair<Variable const*, std::vector<arangodb::basics::AttributeName>>&,
      bool allowIndexedAccess = false) const;

  /// @brief whether or not a node is simple enough to be used in a simple
  /// expression
  /// this may also set the FLAG_SIMPLE flag for the node
  bool isSimple() const;

  /// @brief whether or not a node has a constant value
  /// this may also set the FLAG_CONSTANT or the FLAG_DYNAMIC flags for the node
  bool isConstant() const;

  /// @brief whether or not a node will use V8 internally
  /// this may also set the FLAG_V8 flag for the node
  bool willUseV8() const;

  /// @brief whether or not a node's filter condition can be used inside a
  /// TraversalNode
  bool canBeUsedInFilter(bool isOneShard) const;

  /// @brief whether or not a node is a simple comparison operator
  bool isSimpleComparisonOperator() const;

  /// @brief whether or not a node is a comparison operator
  bool isComparisonOperator() const;

  /// @brief whether or not a node is an array comparison operator
  bool isArrayComparisonOperator() const;

  /// @brief whether or not a node (and its subnodes) can safely be executed on
  /// a DB server
  bool canRunOnDBServer(bool isOneShard) const;

  /// @brief whether or not an object's keys must be checked for uniqueness
  bool mustCheckUniqueness() const;

  /// @brief whether or not a node (and its subnodes) is deterministic
  bool isDeterministic() const;

  /// @brief whether or not a node (and its subnodes) is cacheable
  bool isCacheable() const;

  /// @brief whether or not a node (and its subnodes) may contain a call to a
  /// user-defined function
  bool callsUserDefinedFunction() const;

  /// @brief whether or not a node (and its subnodes) may contain a call to a
  /// a function or a user-defined function
  bool callsFunction() const;

  /// @brief iterates whether a node of type "searchType" can be found
  bool containsNodeType(AstNodeType searchType) const;

  /// @brief return the number of members
  size_t numMembers() const noexcept;

  /// @brief reserve space for members
  void reserve(size_t n);

  /// @brief add a member to the node
  void addMember(AstNode* node);

  /// @brief add a member to the node
  void addMember(AstNode const* node);

  /// @brief change a member of the node
  void changeMember(size_t i, AstNode* node);

  /// @brief remove a member from the node
  void removeMemberUnchecked(size_t i);

  /// @brief remove a member from the node while breaking members ordering.
  /// Faster than removeMemberUnchecked
  void removeMemberUncheckedUnordered(size_t i);

  /// @brief return a member of the node
  AstNode* getMember(size_t i) const;

  /// @brief return a member of the node
  AstNode* getMemberUnchecked(size_t i) const noexcept;

  template<typename Func>
  void sortMembers(Func&& func) {
    std::sort(members.begin(), members.end(), std::forward<Func>(func));
  }

  template<typename Func>
  void partitionMembers(Func&& func) {
    std::partition(members.begin(), members.end(), std::forward<Func>(func));
  }

  /// @brief reduces the number of members of the node
  void reduceMembers(size_t i);

  void clearMembers();

  /// @brief mark a < or <= operator to definitely exclude the "null" value
  /// this is only for optimization purposes
  void setExcludesNull(bool v = true);

  /// @brief check if an < or <= operator definitely excludes the "null" value
  /// this is only for optimization purposes
  bool getExcludesNull() const noexcept;

  /// @brief set the node's value type
  void setValueType(AstNodeValueType type);

  /// @brief check whether this node value is of expectedType
  bool isValueType(AstNodeValueType expectedType) const noexcept;

  /// @brief return the bool value of a node
  bool getBoolValue() const noexcept;

  /// @brief set the bool value of a node
  void setBoolValue(bool v);

  /// @brief return the int value of a node
  /// this will return 0 for all non-value nodes and for all non-int value
  /// nodes!!
  int64_t getIntValue() const;

  /// @brief return the int value stored for a node, regardless of the node type
  int64_t getIntValue(bool) const noexcept;

  /// @brief set the int value of a node
  void setIntValue(int64_t v);

  /// @brief return the double value of a node
  double getDoubleValue() const;

  /// @brief set the string value of a node
  void setDoubleValue(double v);

  /// @brief return the string value of a node
  char const* getStringValue() const;

  /// @brief return the string value of a node
  size_t getStringLength() const;

  /// @brief set the string value of a node
  void setStringValue(char const* v, size_t length);

  /// @brief whether the string value of this node is equal to other
  ///        ignoring case
  bool stringEqualsCaseInsensitive(std::string const& other) const;

  /// @brief whether or not a string is equal to another
  bool stringEquals(std::string const& other) const;

  /// @brief return the data value of a node
  void* getData() const noexcept;

  /// @brief set the data value of a node
  void setData(void* v);

  /// @brief set the data value of a node
  void setData(void const* v);

  /// @brief clone a node, recursively
  AstNode* clone(Ast*) const;

  /// @brief append a string representation of the node into a string buffer
  /// the string representation does not need to be JavaScript-compatible
  /// except for node types NODE_TYPE_VALUE, NODE_TYPE_ARRAY and
  /// NODE_TYPE_OBJECT (only for objects that do not contain dynamic attributes)
  /// note that this may throw and that the caller is responsible for
  /// catching the error
  void stringify(std::string& buffer, bool failIfLong) const;

  /// note that this may throw and that the caller is responsible for
  /// catching the error
  std::string toString() const;

  /// @brief If the node has not been marked finalized, mark its subtree so.
  /// If it runs into a finalized node, it assumes the whole subtree beneath
  /// it is marked already and exits early; otherwise it will finalize the node
  /// and recurse on its subtree.
  static void markFinalized(AstNode* subtreeRoot) noexcept;

  /// @brief sets the computed value pointer.
  void setComputedValue(uint8_t* data);

  /// @brief the node type
  AstNodeType type;

  /// @brief flags for the node
  AstNodeFlagsType mutable flags;

  /// @brief the node value
  AstNodeValue value;

 private:
  /// @brief helper for building flags
  template<typename... Args>
  static std::underlying_type<AstNodeFlagType>::type makeFlags(
      AstNodeFlagType flag, Args... args) noexcept;

  static std::underlying_type<AstNodeFlagType>::type makeFlags() noexcept;

  void computeValue(arangodb::velocypack::Builder& builder) const;
  void freeComputedValue() noexcept;

  /// @brief stringify the value of a node into a string buffer.
  /// this method is used when generating JavaScript code for the node!
  /// this creates an equivalent to what JSON.stringify() would do
  void appendValue(std::string& buffer) const;

  /// @brief precomputed VPack value (used when executing expressions)
  uint8_t mutable* _computedValue;

  /// @brief the node's sub nodes
  std::vector<AstNode*> members{};
};

template<bool resolveAttributeAccess = true>
int compareAstNodes(AstNode const* lhs, AstNode const* rhs, bool compareUtf8);

struct AstNodeValueHash {
  size_t operator()(AstNode const* value) const noexcept;
};

struct AstNodeValueEqual {
  bool operator()(AstNode const* lhs, AstNode const* rhs) const;
};
}  // namespace aql
}  // namespace arangodb

/// @brief append the AstNode to an output stream
std::ostream& operator<<(std::ostream&, arangodb::aql::AstNode const*);
std::ostream& operator<<(std::ostream&, arangodb::aql::AstNode const&);

/** README
 * Typically, the pattern should be that once an AstNode is created and inserted
 * into the tree, it is immutable. Currently there are some places where this
 * is not the case, so we cannot currently enforce it via typing.
 *
 * Instead, we have a flag which we set and check only in maintainer mode,
 * `FLAG_FINALIZED` to signal that the node should not change state. This allows
 * us to take a more functional style approach to building and modifying the
 * Ast, allowing us to simply point to the same node multiple times instead of
 * creating several copies when we need to e.g. distribute an AND operation.
 *
 * If you need to modify a node, try to replace it with a new one. You can
 * create a copy-for-modify node via `Ast::shallowCopyForModify`. You can then
 * set it to be finalized when it goes out of scope using
 * `TRI_DEFER(FINALIZE_SUBTREE(node))`.
 *
 * If you cannot follow this pattern, for instance if you do not have access
 * to the Ast or to the node's ancestors, then you might be able to get away
 * with temporarily unlocking the node and modifying it in place. This could
 * create correctness issues in our optmization code though, so do this with
 * extreme caution. The correct way to do this is via the
 * `TEMPORARILY_UNLOCK_NODE(node)` macro. It will relock the node when the macro
 * instance goes out of scope.
 */

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
#define FINALIZE_SUBTREE(n) \
  arangodb::aql::AstNode::markFinalized(const_cast<arangodb::aql::AstNode*>(n))
#define FINALIZE_SUBTREE_CONDITIONAL(n, b) \
  if (b) {                                 \
    FINALIZE_SUBTREE(n);                   \
  }
#define TEMPORARILY_UNLOCK_NODE(n)                                      \
  bool wasFinalizedAlready =                                            \
      (n)->hasFlag(arangodb::aql::AstNodeFlagType::FLAG_FINALIZED);     \
  if (wasFinalizedAlready) {                                            \
    (n)->flags =                                                        \
        ((n)->flags & ~arangodb::aql::AstNodeFlagType::FLAG_FINALIZED); \
  }                                                                     \
  auto sg = arangodb::scopeGuard([&]() noexcept {                       \
    FINALIZE_SUBTREE_CONDITIONAL(n, wasFinalizedAlready);               \
  })
#else
#define FINALIZE_SUBTREE(n) \
  while (0) {               \
    (void)(n);              \
  }                         \
  do {                      \
  } while (0)
#define FINALIZE_SUBTREE_CONDITIONAL(n, b) \
  while (0) {                              \
    (void)(n);                             \
  }                                        \
  do {                                     \
  } while (0)
#define TEMPORARILY_UNLOCK_NODE(n) \
  while (0) {                      \
    (void)(n);                     \
  }                                \
  do {                             \
  } while (0)
#endif
