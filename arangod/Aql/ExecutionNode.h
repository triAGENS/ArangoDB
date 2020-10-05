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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

// Execution plans like the one below are made of Nodes that inherit the
// ExecutionNode class as a base class.
//
// clang-format off
//
// Execution plan:
//  Id   NodeType                  Est.   Comment
//   1   SingletonNode                1   * ROOT
//   2   EnumerateCollectionNode   6400     - FOR d IN coll   /* full collection scan */
//   3   CalculationNode           6400       - LET #1 = DISTANCE(d.`lat`, d.`lon`, 0, 0)  /* simple expression */   /* collections used: d : coll */
//   4   SortNode                  6400       - SORT #1 ASC
//   5   LimitNode                    5       - LIMIT 0, 5
//   6   ReturnNode                   5       - RETURN d
//
// clang-format on
//
// Even though the Singleton Node has a label saying it is the "ROOT" node it
// is not in our definiton. Root Nodes are leaf nodes (at the bottom of the
// list).
//
// To get down (direction to root) from 4 to 5 you need to call getFirstParent
// on the SortNode(4) to receive a pointer to the LimitNode(5). If you want to
// go up from 5 to 4 (away from root) you need to call getFirstDependency at
// the LimitNode (5) to get a pointer to the SortNode(4).
//
// For most maybe all operations you will only need to operate on the
// Dependencies the parents will be updated automatically.
//
// If you wish to unlink (remove) or replace a node you should do it by using
// one of the plans operations.

#ifndef ARANGOD_AQL_EXECUTION_NODE_H
#define ARANGOD_AQL_EXECUTION_NODE_H 1

#include <memory>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "Aql/CollectionAccessingNode.h"
#include "Aql/CostEstimate.h"
#include "Aql/DocumentProducingNode.h"
#include "Aql/ExecutionNodeId.h"
#include "Aql/RegisterInfos.h"
#include "Aql/IndexHint.h"
#include "Aql/Variable.h"
#include "Aql/WalkerWorker.h"
#include "Aql/types.h"
#include "Basics/Common.h"
#include "Basics/TypeTraits.h"
#include "Basics/Identifier.h"
#include "Containers/HashSet.h"

namespace arangodb {
namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack

class Index;

namespace aql {
class Ast;
struct Collection;
class ExecutionBlock;
class ExecutionEngine;
class ExecutionNode;
class ExecutionPlan;
class RegisterInfos;
class Expression;
class RedundantCalculationsReplacer;
template<typename T> struct RegisterPlanWalkerT;
using RegisterPlanWalker = RegisterPlanWalkerT<ExecutionNode>;
template<typename T> struct RegisterPlanT;
using RegisterPlan = RegisterPlanT<ExecutionNode>;
struct Variable;

/// @brief sort element, consisting of variable, sort direction, and a possible
/// attribute path to dig into the document

struct SortElement {
  Variable const* var;
  bool ascending;
  std::vector<std::string> attributePath;

  SortElement(Variable const* v, bool asc);

  SortElement(Variable const* v, bool asc, std::vector<std::string> const& path);

  /// @brief stringify a sort element. note: the output of this should match the
  /// stringification output of an AstNode for an attribute access
  /// (e.g. foo.bar => $0.bar)
  std::string toString() const;
};

typedef std::vector<SortElement> SortElementVector;

using VariableIdSet = std::set<VariableId>;

/// @brief class ExecutionNode, abstract base class of all execution Nodes
class ExecutionNode {
  /// @brief node type
  friend class ExecutionBlock;
  // Needs to inject sensitive RegisterInformation
  friend RegisterPlanWalkerT<ExecutionNode>;
  // We need this to replan the registers within the QuerySnippet.
  // otherwise the local gather node might delete the sorting register...
  friend class QuerySnippet;

 public:
  enum NodeType : int {
    SINGLETON = 1,
    ENUMERATE_COLLECTION = 2,
    // INDEX_RANGE          =  3, // not used anymore
    ENUMERATE_LIST = 4,
    FILTER = 5,
    LIMIT = 6,
    CALCULATION = 7,
    SUBQUERY = 8,
    SORT = 9,
    COLLECT = 10,
    SCATTER = 11,
    GATHER = 12,
    REMOTE = 13,
    INSERT = 14,
    REMOVE = 15,
    REPLACE = 16,
    UPDATE = 17,
    RETURN = 18,
    NORESULTS = 19,
    DISTRIBUTE = 20,
    UPSERT = 21,
    TRAVERSAL = 22,
    INDEX = 23,
    SHORTEST_PATH = 24,
    K_SHORTEST_PATHS = 25,
    REMOTESINGLE = 26,
    ENUMERATE_IRESEARCH_VIEW = 27,
    DISTRIBUTE_CONSUMER = 28,
    SUBQUERY_START = 29,
    SUBQUERY_END = 30,
    MATERIALIZE = 31,
    ASYNC = 32,
    MUTEX = 33,
    READALL = 34,

    MAX_NODE_TYPE_VALUE
  };

  ExecutionNode() = delete;
  ExecutionNode(ExecutionNode const&) = delete;
  ExecutionNode& operator=(ExecutionNode const&) = delete;

 protected:
  /// @brief Clone constructor, used for constructors of derived classes.
  /// Does not clone recursively, does not clone properties (`other.plan()` is
  /// expected to be the same as `plan)`, and does not register this node in the
  /// plan.
  ExecutionNode(ExecutionPlan& plan, ExecutionNode const& other);

 public:
  /// @brief constructor using an id
  ExecutionNode(ExecutionPlan* plan, size_t id) { TRI_ASSERT(false); }
  ExecutionNode(ExecutionPlan* plan, ExecutionNodeId id);

  /// @brief constructor using a VPackSlice
  ExecutionNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& slice);

  /// @brief destructor, free dependencies
  virtual ~ExecutionNode() = default;

 public:
  /// @brief factory from JSON
  static ExecutionNode* fromVPackFactory(ExecutionPlan* plan,
                                         arangodb::velocypack::Slice const& slice);

  /// @brief cast an ExecutionNode to a specific sub-type
  /// in maintainer mode, this function will perform a dynamic_cast and abort
  /// the program if the cast is invalid. in release mode, this function will
  /// perform a static_cast and will not abort the program
  template <typename T, typename FromType>
  static inline T castTo(FromType node) noexcept {
    static_assert(std::is_pointer<T>::value,
                  "invalid type passed into ExecutionNode::castTo");
    static_assert(std::is_pointer<FromType>::value,
                  "invalid type passed into ExecutionNode::castTo");
    static_assert(std::remove_pointer<FromType>::type::IsExecutionNode,
                  "invalid type passed into ExecutionNode::castTo");

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    T result = dynamic_cast<T>(node);
    TRI_ASSERT(result != nullptr);
    return result;
#else
    // At least GraphNode is virtually inherited by its subclasses. We have to
    // use dynamic_cast for these types.
    if constexpr (can_static_cast_v<FromType, T>) {
      return static_cast<T>(node);
    } else {
      return dynamic_cast<T>(node);
    }
#endif
  }

  /// @brief return the node's id
  ExecutionNodeId id() const;

  /// @brief return the type of the node
  virtual NodeType getType() const = 0;

  /// @brief resolve nodeType to a string.
  static std::string const& getTypeString(NodeType type);

  /// @brief return the type name of the node
  std::string const& getTypeString() const;

  /// @brief checks whether we know a type of this kind; throws exception if
  /// not.
  static void validateType(int type);

  /// @brief whether or not a node is located inside a subquery
  bool isInSubquery() const;

  /// @brief add a dependency
  void addDependency(ExecutionNode*);

  /// @brief add a parent
  void addParent(ExecutionNode*);

  /// @brief swap the first dependency
  ///        use with care, will modify the plan
  void swapFirstDependency(ExecutionNode* node);

  /// @brief get all dependencies
  TEST_VIRTUAL std::vector<ExecutionNode*> const& getDependencies() const;

  /// @brief returns the first dependency, or a nullptr if none present
  ExecutionNode* getFirstDependency() const;

  /// @brief whether or not the node has a dependency
  bool hasDependency() const;

  /// @brief add the node dependencies to a vector
  void dependencies(std::vector<ExecutionNode*>& result) const;

  /// @brief get all parents
  std::vector<ExecutionNode*> getParents() const;

  /// @brief whether or not the node has a parent
  bool hasParent() const;

  /// @brief whether or not the node has any ancestor (parent at any distance)
  /// of this type
  bool hasParentOfType(ExecutionNode::NodeType type) const;

  /// @brief returns the first parent, or a nullptr if none present
  ExecutionNode* getFirstParent() const;

  /// @brief add the node parents to a vector
  void parents(std::vector<ExecutionNode*>& result) const;

  /// @brief get the singleton node of the node
  ExecutionNode const* getSingleton() const;
  ExecutionNode* getSingleton();

  /// @brief get the node and its dependencies as a vector
  void getDependencyChain(std::vector<ExecutionNode*>& result, bool includeSelf);

  /// @brief inspect one index; only skiplist indices which match attrs in
  /// sequence.
  /// returns a a qualification how good they match;
  ///      match->index==nullptr means no match at all.
  enum MatchType {
    FORWARD_MATCH,
    REVERSE_MATCH,
    NOT_COVERED_IDX,
    NOT_COVERED_ATTR,
    NO_MATCH
  };

  /// @brief make a new node the (only) parent of the node
  void setParent(ExecutionNode* p);

  /// @brief replace a dependency, returns true if the pointer was found and
  /// replaced, please note that this does not delete oldNode!
  bool replaceDependency(ExecutionNode* oldNode, ExecutionNode* newNode);

  /// @brief remove a dependency, returns true if the pointer was found and
  /// removed, please note that this does not delete ep!
  bool removeDependency(ExecutionNode*);

  /// @brief remove all dependencies for the given node
  void removeDependencies();

  /// @brief creates corresponding ExecutionBlock
  virtual std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const& cache) const = 0;

  /// @brief clone execution Node recursively, this makes the class abstract
  virtual ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                               bool withProperties) const = 0;

  /// @brief execution Node clone utility to be called by derives
  /// @return pointer to a registered node owned by a plan
  ExecutionNode* cloneHelper(std::unique_ptr<ExecutionNode> Other,
                             bool withDependencies, bool withProperties) const;

  void cloneWithoutRegisteringAndDependencies(ExecutionPlan& plan, ExecutionNode& other,
                                              bool withProperties) const;

  /// @brief helper for cloning, use virtual clone methods for dependencies
  void cloneDependencies(ExecutionPlan* plan, ExecutionNode* theClone, bool withProperties) const;
  
  // clone register plan of dependency, needed when inserting nodes after planning
  void cloneRegisterPlan(ExecutionNode* dependency);

  /// @brief check equality of ExecutionNodes
  virtual bool isEqualTo(ExecutionNode const& other) const;

  /// @brief invalidate the cost estimate for the node and its dependencies
  virtual void invalidateCost();

  /// @brief estimate the cost of the node . . .
  /// does not recalculate the estimate if already calculated
  CostEstimate getCost() const;

  /// @brief walk a complete execution plan recursively
  bool walk(WalkerWorkerBase<ExecutionNode>& worker);

  bool walkSubqueriesFirst(WalkerWorkerBase<ExecutionNode>& worker);

  /// serialize parents of each node (used in the explainer)
  static constexpr unsigned SERIALIZE_PARENTS = 1;
  /// include estimate cost  (used in the explainer)
  static constexpr unsigned SERIALIZE_ESTIMATES = 1 << 1;
  /// Print all ExecutionNode information required in cluster snippets
  static constexpr unsigned SERIALIZE_DETAILS = 1 << 2;
  /// include additional function info for explain
  static constexpr unsigned SERIALIZE_FUNCTIONS = 1 << 3;
  /// include addition information of the register plan for explain
  static constexpr unsigned SERIALIZE_REGISTER_INFORMATION = 1 << 4;

  /// @brief toVelocyPack, export an ExecutionNode to VelocyPack
  void toVelocyPack(arangodb::velocypack::Builder&, unsigned flags, bool keepTopLevelOpen) const;

  /// @brief toVelocyPack
  virtual void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                                  std::unordered_set<ExecutionNode const*>& seen) const = 0;

  /** Variables used and set are disjunct!
   *   Variables that are read from must be returned by the
   *   UsedHere functions and variables that are filled by
   *   the corresponding ExecutionBlock must be added in
   *   the SetHere functions.
   */

  /// @brief getVariablesUsedHere, modifying the set in-place
  virtual void getVariablesUsedHere(VarSet& vars) const;

  /// @brief getVariablesSetHere
  virtual std::vector<Variable const*> getVariablesSetHere() const;

  /// @brief getVariableIdsUsedHere
  ::arangodb::containers::HashSet<VariableId> getVariableIdsUsedHere() const;

  /// @brief tests whether the node sets one of the passed variables
  bool setsVariable(VarSet const& which) const;

  /// @brief setVarsUsedLater
  void setVarsUsedLater(VarSetStack varStack);

  /// @brief getVarsUsedLater, this returns the set of variables that will be
  /// used later than this node, i.e. in the repeated parents.
  auto getVarsUsedLater() const noexcept -> VarSet const&;

  /// @brief Stack of getVarsUsedLater, needed for spliced subqueries. Index 0
  /// corresponds to the outermost spliced subquery, up to either the top level
  /// or a classic subquery. While the last entry corresponds to the current
  /// level and is the same as getVarsUsedLater().
  /// Is never empty (after the VarUsageFinder ran / if _varUsageValid is true).
  auto getVarsUsedLaterStack() const noexcept -> VarSetStack const&;

  /// @brief setVarsValid
  void setVarsValid(VarSetStack varStack);

  /// @brief set regs to be deleted
  void setRegsToClear(RegIdSet toClear);

  /// @brief set regs to be kept
  void setRegsToKeep(RegIdSetStack toKeep);

  /// @brief getVarsValid, this returns the set of variables that is valid
  /// for items leaving this node, this includes those that will be set here
  /// (see getVariablesSetHere).
  auto getVarsValid() const noexcept -> VarSet const&;

  /// @brief Stack of getVarsValid, needed for spliced subqueries. Index 0
  /// corresponds to the outermost spliced subquery, up to either the top level
  /// or a classic subquery. While the last entry corresponds to the current
  /// level and is the same as getVarsValid().
  /// Is never empty (after the VarUsageFinder ran / if _varUsageValid is true).
  auto getVarsValidStack() const noexcept -> VarSetStack const&;

  /// @brief setVarUsageValid
  void setVarUsageValid();

  /// @brief invalidateVarUsage
  void invalidateVarUsage();

  /// @brief whether or not the subquery is deterministic
  virtual bool isDeterministic();

  /// @brief whether or not the node is a data modification node
  virtual bool isModificationNode() const;

  ExecutionPlan const* plan() const;

  ExecutionPlan* plan();

  /// @brief static analysis
  void planRegisters(ExecutionNode* super = nullptr, ExplainRegisterPlan = ExplainRegisterPlan::No);

  /// @brief get RegisterPlan
  std::shared_ptr<RegisterPlan> getRegisterPlan() const;

  /// @brief get depth
  int getDepth() const;

  /// @brief get registers to clear
  RegIdSet const& getRegsToClear() const;

  /// @brief check if a variable will be used later
  bool isVarUsedLater(Variable const* variable) const;

  /// @brief whether or not the node is in an inner loop
  bool isInInnerLoop() const;

  /// @brief get the surrounding loop
  ExecutionNode const* getLoop() const;

  bool isInSplicedSubquery() const noexcept;

  void setIsInSplicedSubquery(bool) noexcept;

  [[nodiscard]] static bool isIncreaseDepth(NodeType type);
  [[nodiscard]] bool isIncreaseDepth() const;
  [[nodiscard]] static bool alwaysCopiesRows(NodeType type);
  [[nodiscard]] bool alwaysCopiesRows() const;

 protected:
  /// @brief set the id, use with care! The purpose is to use a cloned node
  /// together with the original in the same plan.
  void setId(ExecutionNodeId id);

  /// @brief this actually estimates the costs as well as the number of items
  /// coming out of the node
  virtual CostEstimate estimateCost() const = 0;

  /// @brief factory for sort elements
  static void getSortElements(SortElementVector& elements, ExecutionPlan* plan,
                              arangodb::velocypack::Slice const& slice, char const* which);

  /// @brief toVelocyPackHelper, for a generic node
  void toVelocyPackHelperGeneric(arangodb::velocypack::Builder&, unsigned flags,
                                 std::unordered_set<ExecutionNode const*>& seen) const;

  auto getRegsToKeepStack() const -> RegIdSetStack;

  RegisterId variableToRegisterId(Variable const*) const;

  RegisterId variableToRegisterOptionalId(Variable const* var) const;

  RegisterInfos createRegisterInfos(RegIdSet readableInputRegisters,
                                    RegIdSet writableOutputRegisters) const;

  RegisterCount getNrInputRegisters() const;

  RegisterCount getNrOutputRegisters() const;

 protected:
  /// @brief node id
  ExecutionNodeId _id;

  /// @brief our dependent nodes
  std::vector<ExecutionNode*> _dependencies;

  /// @brief our parent nodes
  std::vector<ExecutionNode*> _parents;

  /// @brief cost estimate for the node
  CostEstimate mutable _costEstimate;

  /// @brief _varsUsedLaterStack and _varsValidStack.
  /// The back() always corresponds to the current node, while the lower indexes
  /// correspond to containing spliced subqueries, up to either the top level
  /// or a containing SubqueryNode. Is thus never empty (after VarUsageFinder
  /// ran, i.e. _varUsageValid is true).
  /// VarsUsedLater are variables that are used by any of this node's ancestors.
  /// VarsValid are variables that are either set here, of by any of its
  /// descendants.
  /// Both are set by calling ExecutionPlan::findVarUsage. After that,
  /// _varUsageValid is true.
  VarSetStack _varsUsedLaterStack;
  VarSetStack _varsValidStack;

  /// @brief depth of the current frame, will be filled in by planRegisters
  unsigned int _depth;

  /// @brief whether or not _varsUsedLater and _varsValid are actually valid
  bool _varUsageValid;

  bool _isInSplicedSubquery;

  /// @brief _plan, the ExecutionPlan object
  ExecutionPlan* _plan;

  /// @brief info about variables, filled in by planRegisters
  std::shared_ptr<RegisterPlan> _registerPlan;

  /// @brief the following contains the registers which should be cleared
  /// just before this node hands on results. This is computed during
  /// the static analysis for each node using the variable usage in the plan.
  RegIdSet _regsToClear;

  /// @brief contains the registers which should be copied into the next row
  /// This is computed during the static analysis for each node using the
  /// variable usage in the plan.
  RegIdSetStack _regsToKeepStack;

 public:
  /// @brief used as "type traits" for ExecutionNodes and derived classes
  static constexpr bool IsExecutionNode = true;
};

/// @brief class SingletonNode
class SingletonNode : public ExecutionNode {
  friend class ExecutionBlock;

  /// @brief constructor with an id
 public:
  SingletonNode(ExecutionPlan* plan, ExecutionNodeId id);

  SingletonNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final {
    return cloneHelper(std::make_unique<SingletonNode>(plan, _id),
                       withDependencies, withProperties);
  }

  /// @brief the cost of a singleton is 1
  CostEstimate estimateCost() const override final;
};

/// @brief class EnumerateCollectionNode
class EnumerateCollectionNode : public ExecutionNode,
                                public DocumentProducingNode,
                                public CollectionAccessingNode {
  friend class ExecutionNode;
  friend class ExecutionBlock;

  /// @brief constructor with a vocbase and a collection name
 public:
  EnumerateCollectionNode(ExecutionPlan* plan, ExecutionNodeId id,
                          aql::Collection const* collection, Variable const* outVariable,
                          bool random, IndexHint const& hint);

  EnumerateCollectionNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief the cost of an enumerate collection node is a multiple of the cost
  /// of its unique dependency
  CostEstimate estimateCost() const override final;

  /// @brief getVariablesSetHere
  std::vector<Variable const*> getVariablesSetHere() const override final;

  /// @brief the node is only non-deterministic if it uses a random sort order
  bool isDeterministic() override final;

  /// @brief enable random iteration of documents in collection
  void setRandom();

  /// @brief user hint regarding which index ot use
  IndexHint const& hint() const;

 private:
  /// @brief whether or not we want random iteration
  bool _random;

  /// @brief a possible hint from the user regarding which index to use
  IndexHint _hint;
};

/// @brief class EnumerateListNode
class EnumerateListNode : public ExecutionNode {
  friend class ExecutionNode;
  friend class ExecutionBlock;
  friend class RedundantCalculationsReplacer;

 public:
  EnumerateListNode(ExecutionPlan* plan, ExecutionNodeId id,
                    Variable const* inVariable, Variable const* outVariable);

  EnumerateListNode(ExecutionPlan*, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief the cost of an enumerate list node
  CostEstimate estimateCost() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override final;

  /// @brief getVariablesSetHere
  std::vector<Variable const*> getVariablesSetHere() const override final;

  /// @brief return in variable
  Variable const* inVariable() const;

  /// @brief return out variable
  Variable const* outVariable() const;

 private:
  /// @brief input variable to read from
  Variable const* _inVariable;

  /// @brief output variable to write to
  Variable const* _outVariable;
};

/// @brief class LimitNode
class LimitNode : public ExecutionNode {
  friend class ExecutionBlock;

 public:
  LimitNode(ExecutionPlan* plan, ExecutionNodeId id, size_t offset, size_t limit);

  LimitNode(ExecutionPlan*, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief estimateCost
  CostEstimate estimateCost() const override final;

  /// @brief tell the node to fully count what it will limit
  void setFullCount();

  bool fullCount() const noexcept;

  /// @brief return the offset value
  size_t offset() const;

  /// @brief return the limit value
  size_t limit() const;

 private:
  /// @brief the offset
  size_t _offset;

  /// @brief the limit
  size_t _limit;

  /// @brief whether or not the node should fully count what it limits
  bool _fullCount;
};

/// @brief class CalculationNode
class CalculationNode : public ExecutionNode {
  friend class ExecutionNode;
  friend class ExecutionBlock;
  friend class RedundantCalculationsReplacer;

 public:
  CalculationNode(ExecutionPlan* plan, ExecutionNodeId id,
                  std::unique_ptr<Expression> expr, Variable const* outVariable);

  CalculationNode(ExecutionPlan*, arangodb::velocypack::Slice const& base);

  ~CalculationNode();

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief return out variable
  Variable const* outVariable() const;

  /// @brief return the expression. never a nullptr!
  Expression* expression() const;

  /// @brief estimateCost
  CostEstimate estimateCost() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override final;

  /// @brief getVariablesSetHere
  virtual std::vector<Variable const*> getVariablesSetHere() const override final;

  bool isDeterministic() override final;

 private:
  /// @brief output variable to write to
  Variable const* _outVariable;

  /// @brief we need to have an expression and where to write the result
  std::unique_ptr<Expression> _expression;
};

/// @brief class SubqueryNode
class SubqueryNode : public ExecutionNode {
  friend class ExecutionNode;
  friend class ExecutionBlock;

 public:
  SubqueryNode(ExecutionPlan*, arangodb::velocypack::Slice const& base);

  SubqueryNode(ExecutionPlan* plan, ExecutionNodeId id, ExecutionNode* subquery,
               Variable const* outVariable);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief invalidate the cost estimate for the node and its dependencies
  void invalidateCost() override;

  /// @brief return the out variable
  Variable const* outVariable() const;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief this is true iff the subquery contains a data-modification operation
  ///        NOTE that this is tested recursively, that is, if this subquery contains
  ///        a subquery that contains a modification operation, this is true too.
  bool isModificationNode() const override;

  /// @brief getter for subquery
  ExecutionNode* getSubquery() const;

  /// @brief setter for subquery
  void setSubquery(ExecutionNode* subquery, bool forceOverwrite);

  /// @brief estimateCost
  CostEstimate estimateCost() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override final;

  /// @brief getVariablesSetHere
  std::vector<Variable const*> getVariablesSetHere() const override final;

  /// @brief replace the out variable, so we can adjust the name.
  void replaceOutVariable(Variable const* var);

  bool isDeterministic() override final;

  bool isConst();
  bool mayAccessCollections();

 private:
  /// @brief we need to have an expression and where to write the result
  ExecutionNode* _subquery;

  /// @brief variable to write to
  Variable const* _outVariable;
};

/// @brief class FilterNode
class FilterNode : public ExecutionNode {
  friend class ExecutionBlock;
  friend class RedundantCalculationsReplacer;

  /// @brief constructors for various arguments, always with offset and limit
 public:
  FilterNode(ExecutionPlan* plan, ExecutionNodeId id, Variable const* inVariable);

  FilterNode(ExecutionPlan*, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief estimateCost
  CostEstimate estimateCost() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override final;

  Variable const* inVariable() const;

 private:
  /// @brief input variable to read from
  Variable const* _inVariable;
};

/// @brief this is an auxilliary struct for processed sort criteria information
struct SortInformation {
  enum Match {
    unequal,                // criteria are unequal
    otherLessAccurate,      // leftmost sort criteria are equal, but other sort
                            // criteria are less accurate than ourselves
    ourselvesLessAccurate,  // leftmost sort criteria are equal, but our own
                            // sort criteria is less accurate than the other
    allEqual                // all criteria are equal
  };

  std::vector<std::tuple<ExecutionNode const*, std::string, bool>> criteria;
  bool isValid = true;
  bool isDeterministic = true;
  bool isComplex = false;

  Match isCoveredBy(SortInformation const& other);
};

/// @brief class ReturnNode
class ReturnNode : public ExecutionNode {
  friend class ExecutionBlock;
  friend class RedundantCalculationsReplacer;

  /// @brief constructors for various arguments, always with offset and limit
 public:
  ReturnNode(ExecutionPlan* plan, ExecutionNodeId id, Variable const* inVariable);

  ReturnNode(ExecutionPlan*, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief tell the node to count the returned values
  void setCount();

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief estimateCost
  CostEstimate estimateCost() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override final;

  Variable const* inVariable() const;

  void inVariable(Variable const* v);

  bool returnInheritedResults() const;

 private:
  /// @brief the variable produced by Return
  Variable const* _inVariable;

  bool _count;
};

/// @brief class NoResultsNode
class NoResultsNode : public ExecutionNode {
  friend class ExecutionBlock;

  /// @brief constructor with an id
 public:
  NoResultsNode(ExecutionPlan* plan, ExecutionNodeId id);

  NoResultsNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief the cost of a NoResults is 0
  CostEstimate estimateCost() const override final;
};


/// @brief class AsyncNode
class AsyncNode : public ExecutionNode {
  friend class ExecutionBlock;

  /// @brief constructor with an id
 public:
  AsyncNode(ExecutionPlan* plan, ExecutionNodeId id);

  AsyncNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief return the type of the node
  NodeType getType() const override final;

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder&, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief the cost of a AsyncNode is whatever is 0
  CostEstimate estimateCost() const override final;
};

namespace materialize {
class MaterializeNode : public ExecutionNode {
 protected:
  MaterializeNode(ExecutionPlan* plan, ExecutionNodeId id,
                  aql::Variable const& inDocId, aql::Variable const& outVariable);

  MaterializeNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

 public:
  /// @brief return the type of the node
  NodeType getType() const override final { return ExecutionNode::MATERIALIZE; }

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override = 0;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override = 0;

  CostEstimate estimateCost() const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override;

  /// @brief getVariablesSetHere
  std::vector<Variable const*> getVariablesSetHere() const override final;

  /// @brief return out variable
  arangodb::aql::Variable const& outVariable() const noexcept {
    return *_outVariable;
  }
 protected:
  template <typename T>
  auto getReadableInputRegisters(T collectionSource, RegisterId inNmDocId) const
      -> RegIdSet;

 protected:
  /// @brief input variable non-materialized document ids
  aql::Variable const* _inNonMaterializedDocId;

  /// @brief the variable produced by materialization
  Variable const* _outVariable;
};

template <typename T>
auto MaterializeNode::getReadableInputRegisters(T const collectionSource,
                                                RegisterId const inNmDocId) const
    -> RegIdSet {
  if constexpr (std::is_same_v<T, RegisterId>) {
    return RegIdSet{collectionSource, inNmDocId};
  } else {
    return RegIdSet{inNmDocId};
  }
}

class MaterializeMultiNode : public MaterializeNode {
 public:
  MaterializeMultiNode(ExecutionPlan* plan, ExecutionNodeId id,
                       aql::Variable const& inColPtr, aql::Variable const& inDocId,
                       aql::Variable const& outVariable);

  MaterializeMultiNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override final;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;

  /// @brief getVariablesUsedHere, modifying the set in-place
  void getVariablesUsedHere(VarSet& vars) const override final;

 private:
  /// @brief input variable non-materialized collection ids
  aql::Variable const* _inNonMaterializedColPtr;
};

class MaterializeSingleNode : public MaterializeNode, public CollectionAccessingNode {
 public:
  MaterializeSingleNode(ExecutionPlan* plan, ExecutionNodeId id,
                        aql::Collection const* collection, aql::Variable const& inDocId,
                        aql::Variable const& outVariable);

  MaterializeSingleNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

  /// @brief export to VelocyPack
  void toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override final;

  /// @brief creates corresponding ExecutionBlock
  std::unique_ptr<ExecutionBlock> createBlock(
      ExecutionEngine& engine,
      std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const override final;

  /// @brief clone ExecutionNode recursively
  ExecutionNode* clone(ExecutionPlan* plan, bool withDependencies,
                       bool withProperties) const override final;
};

MaterializeNode* createMaterializeNode(ExecutionPlan* plan,
                                       arangodb::velocypack::Slice const& base);

}  // namespace materialize
}  // namespace aql
}  // namespace arangodb

#endif
