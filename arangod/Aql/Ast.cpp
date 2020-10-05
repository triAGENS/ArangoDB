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

#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/StringRef.h>
#include <velocypack/velocypack-aliases.h>

#include "Ast.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/AqlValueMaterializer.h"
#include "Aql/Arithmetic.h"
#include "Aql/AstNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/FixedVarExpressionContext.h"
#include "Aql/Function.h"
#include "Aql/Graphs.h"
#include "Aql/ModificationOptions.h"
#include "Aql/QueryContext.h"
#include "Aql/AqlFunctionsInternalCache.h"
#include "Basics/Exceptions.h"
#include "Basics/StringUtils.h"
#include "Basics/tri-strings.h"
#include "Basics/tryEmplaceHelper.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Containers/SmallVector.h"
#include "Graph/Graph.h"
#include "Transaction/Helpers.h"
#include "Utils/CollectionNameResolver.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace {

auto doNothingVisitor = [](AstNode const*) {};
   
[[noreturn]] void throwFormattedError(arangodb::aql::QueryContext& query, int code, char const* details) {
  std::string msg = arangodb::aql::QueryWarnings::buildFormattedString(code, details);
  query.warnings().registerError(code, msg.c_str());
}

/**
 * @brief Register the given datasource with the given accesstype in the query.
 *        Will be noop if the datasource is already used and has the same or
 * higher accessType. Will upgrade the accessType if datasource is used with
 * lower one before.
 *
 * @param resolver CollectionNameResolver to identify category
 * @param accessType Access of this Source, NONE/READ/WRITE/EXCLUSIVE
 * @param failIfDoesNotExist If true => throws error im SourceNotFound. False =>
 * Treat non-existing like a collection
 * @param name Name of the datasource
 *
 * @return The Category of this datasource (Collection or View), and a reference
 * to the translated name (cid => name if required).
 */
LogicalDataSource::Category const* injectDataSourceInQuery(
    Ast& ast, arangodb::CollectionNameResolver const& resolver, AccessMode::Type accessType,
    bool failIfDoesNotExist, arangodb::velocypack::StringRef& nameRef) {
  std::string const name = nameRef.toString();
  // NOTE The name may be modified if a numeric collection ID is given instead
  // of a collection Name. Afterwards it will contain the name.
  auto const dataSource = resolver.getDataSource(name);

  if (dataSource == nullptr) {
    // datasource not found...
    if (failIfDoesNotExist) {
      THROW_ARANGO_EXCEPTION_FORMAT(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                    "name: %s", name.c_str());
    }

    // still add datasource to query, simply because the AST will also be built
    // for queries that are parsed-only (e.g. via `db._parse(query);`. In this
    // case it is ok that the datasource does not exist, but we need to track
    // the names of datasources used in the query
    ast.query().collections().add(name, accessType, aql::Collection::Hint::None);

    return LogicalCollection::category();
  }

  // query actual name from datasource... this may be different to the
  // name passed into this function, because the user may have accessed
  // the collection by its numeric id
  auto const& dataSourceName = dataSource->name();

  if (nameRef != name) {
    // name has changed by the lookup, so we need to reserve the collection
    // name on the heap and update our arangodb::velocypack::StringRef
    char* p = ast.resources().registerString(dataSourceName.data(), dataSourceName.size());
    nameRef = arangodb::velocypack::StringRef(p, dataSourceName.size());
  }

  if (dataSource->category() == LogicalCollection::category()) {
    // it's a collection!
    // add datasource to query
    aql::Collection::Hint hint = aql::Collection::Hint::Collection;
    if (ServerState::instance()->isDBServer()) {
      hint = aql::Collection::Hint::Shard;
    }
    ast.query().collections().add(nameRef.toString(), accessType, hint);
    if (nameRef != name) {
      ast.query().collections().add(name, accessType, hint);  // Add collection by ID as well
    }
  } else if (dataSource->category() == LogicalView::category()) {
    // it's a view!
    // add views to the collection list
    // to register them with transaction as well
    ast.query().collections().add(nameRef.toString(), accessType, aql::Collection::Hint::None);

    ast.query().addDataSource(dataSource);

    // Make sure to add all collections now:
    resolver.visitCollections(
        [&ast, accessType](LogicalCollection& col) -> bool {
          ast.query().collections().add(col.name(), accessType, aql::Collection::Hint::Collection);
          return true;
        },
        dataSource->id());
  } else {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "unexpected datasource type");
  }

  return dataSource->category();
}

}  // namespace

/// @brief inverse comparison operators
std::unordered_map<int, AstNodeType> const Ast::NegatedOperators{
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_EQ), NODE_TYPE_OPERATOR_BINARY_NE},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_NE), NODE_TYPE_OPERATOR_BINARY_EQ},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GT), NODE_TYPE_OPERATOR_BINARY_LE},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GE), NODE_TYPE_OPERATOR_BINARY_LT},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LT), NODE_TYPE_OPERATOR_BINARY_GE},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LE), NODE_TYPE_OPERATOR_BINARY_GT},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_IN), NODE_TYPE_OPERATOR_BINARY_NIN},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_NIN), NODE_TYPE_OPERATOR_BINARY_IN}};

/// @brief reverse comparison operators
std::unordered_map<int, AstNodeType> const Ast::ReversedOperators{
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_EQ), NODE_TYPE_OPERATOR_BINARY_EQ},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GT), NODE_TYPE_OPERATOR_BINARY_LT},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_GE), NODE_TYPE_OPERATOR_BINARY_LE},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LT), NODE_TYPE_OPERATOR_BINARY_GT},
    {static_cast<int>(NODE_TYPE_OPERATOR_BINARY_LE), NODE_TYPE_OPERATOR_BINARY_GE}};

Ast::SpecialNodes::SpecialNodes() 
  : NopNode{NODE_TYPE_NOP},
    NullNode{AstNodeValue()},
    FalseNode{AstNodeValue(false)},
    TrueNode{AstNodeValue(true)},
    ZeroNode{AstNodeValue(int64_t(0))},
    EmptyStringNode{AstNodeValue("", uint32_t(0))} {

  NopNode.setFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);
  NullNode.setFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);
  FalseNode.setFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);
  TrueNode.setFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);
  ZeroNode.setFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);
  EmptyStringNode.setFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);

  // the const-away casts are necessary API-wise. however, we are never ever modifying
  // the computed values for these special nodes.
  NullNode.setComputedValue(const_cast<uint8_t*>(VPackSlice::nullSlice().begin()));
  FalseNode.setComputedValue(const_cast<uint8_t*>(VPackSlice::falseSlice().begin()));
  TrueNode.setComputedValue(const_cast<uint8_t*>(VPackSlice::trueSlice().begin()));
  ZeroNode.setComputedValue(const_cast<uint8_t*>(VPackSlice::zeroSlice().begin()));
  EmptyStringNode.setComputedValue(const_cast<uint8_t*>(VPackSlice::emptyStringSlice().begin()));
  
  TRI_ASSERT(NopNode.hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  TRI_ASSERT(NullNode.hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  TRI_ASSERT(FalseNode.hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  TRI_ASSERT(TrueNode.hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  TRI_ASSERT(ZeroNode.hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  TRI_ASSERT(EmptyStringNode.hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
}

/// @brief create the AST
Ast::Ast(QueryContext& query)
    : _query(query),
      _resources(&query.resourceMonitor()),
      _root(nullptr),
      _functionsMayAccessDocuments(false),
      _containsTraversal(false),
      _containsBindParameters(false),
      _containsModificationNode(false),
      _containsParallelNode(false),
      _willUseV8(false) {
  startSubQuery();

  TRI_ASSERT(_root != nullptr);
}

/// @brief destroy the AST
Ast::~Ast() = default;

/// @brief convert the AST into VelocyPack
void Ast::toVelocyPack(VPackBuilder& builder, bool verbose) const {
  {
    VPackArrayBuilder guard(&builder);
    _root->toVelocyPack(builder, verbose);
  }
}

/// @brief add an operation to the AST
void Ast::addOperation(AstNode* node) {
  TRI_ASSERT(_root != nullptr);

  _root->addMember(node);
}

/// @brief find the bottom-most expansion subnodes (if any)
AstNode const* Ast::findExpansionSubNode(AstNode const* current) const {
  while (true) {
    TRI_ASSERT(current->type == NODE_TYPE_EXPANSION);

    if (current->getMember(1)->type != NODE_TYPE_EXPANSION) {
      return current;
    }
    current = current->getMember(1);
  }
}

/// @brief create a node from the velocypack slice
AstNode* Ast::createNode(arangodb::velocypack::Slice slice) {
  return _resources.registerNode(this, slice);
}

/// @brief create an AST passhthru node
/// note: this type of node is only used during parsing and optimized away later
AstNode* Ast::createNodePassthru(AstNode const* what) {
  AstNode* node = createNode(NODE_TYPE_PASSTHRU);

  node->addMember(what);

  return node;
}

/// @brief create an AST example node
AstNode* Ast::createNodeExample(AstNode const* variable, AstNode const* example) {
  if (example == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (example->type != NODE_TYPE_OBJECT && example->type != NODE_TYPE_PARAMETER) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_PARSE,
        "expecting object literal or bind parameter for example");
  }

  AstNode* node = createNode(NODE_TYPE_EXAMPLE);

  node->setData(const_cast<AstNode*>(variable));
  node->addMember(example);

  return node;
}

/// @brief create subquery node
AstNode* Ast::createNodeSubquery() { return createNode(NODE_TYPE_SUBQUERY); }

/// @brief create an AST for (non-view) node
AstNode* Ast::createNodeFor(char const* variableName, size_t nameLength,
                            AstNode const* expression, bool isUserDefinedVariable) {
  if (variableName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_FOR);
  node->reserve(3);

  AstNode* variable = createNodeVariable(variableName, nameLength, isUserDefinedVariable);
  node->addMember(variable);
  node->addMember(expression);
  node->addMember(&_specialNodes.NopNode);

  return node;
}

/// @brief create an AST for (non-view) node, using an existing output variable
AstNode* Ast::createNodeFor(Variable* variable, AstNode const* expression,
                            AstNode const* options) {
  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  AstNode* v = createNode(NODE_TYPE_VARIABLE);
  v->setData(static_cast<void*>(variable));

  AstNode* node = createNode(NODE_TYPE_FOR);
  node->reserve(3);

  node->addMember(v);
  node->addMember(expression);
  node->addMember(options);

  return node;
}

/// @brief create an AST for (view) node, using an existing output variable
AstNode* Ast::createNodeForView(Variable* variable, AstNode const* expression,
                                AstNode const* search, AstNode const* options) {
  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  TRI_ASSERT(search != nullptr);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  AstNode* variableNode = createNode(NODE_TYPE_VARIABLE);
  variableNode->setData(static_cast<void*>(variable));

  AstNode* node = createNode(NODE_TYPE_FOR_VIEW);
  node->reserve(4);

  node->addMember(variableNode);
  node->addMember(expression);
  node->addMember(createNodeFilter(search));
  node->addMember(options);

  return node;
}

/// @brief create an AST let node, without an IF condition
AstNode* Ast::createNodeLet(char const* variableName, size_t nameLength,
                            AstNode const* expression, bool isUserDefinedVariable) {
  if (variableName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_LET);
  node->reserve(2);

  AstNode* variable = createNodeVariable(variableName, nameLength, isUserDefinedVariable);
  node->addMember(variable);
  node->addMember(expression);

  return node;
}

/// @brief create an AST let node, without creating a variable
AstNode* Ast::createNodeLet(AstNode const* variable, AstNode const* expression) {
  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_LET);
  node->reserve(2);

  node->addMember(variable);
  node->addMember(expression);

  return node;
}

/// @brief create an AST let node, with an IF condition
AstNode* Ast::createNodeLet(char const* variableName, size_t nameLength,
                            AstNode const* expression, AstNode const* condition) {
  if (variableName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_LET);
  node->reserve(3);

  AstNode* variable = createNodeVariable(variableName, nameLength, true);
  node->addMember(variable);
  node->addMember(expression);
  node->addMember(condition);

  return node;
}

/// @brief create an AST filter node
AstNode* Ast::createNodeFilter(AstNode const* expression) {
  AstNode* node = createNode(NODE_TYPE_FILTER);
  node->addMember(expression);

  return node;
}

/// @brief create an AST filter node for an UPSERT query
AstNode* Ast::createNodeUpsertFilter(AstNode const* variable, AstNode const* object) {
  AstNode* node = createNode(NODE_TYPE_FILTER);
  AstNode* example = createNodeExample(variable, object);

  node->addMember(example);

  return node;
}

/// @brief create an AST return node
AstNode* Ast::createNodeReturn(AstNode const* expression) {
  AstNode* node = createNode(NODE_TYPE_RETURN);
  node->addMember(expression);

  return node;
}

/// @brief create an AST remove node
AstNode* Ast::createNodeRemove(AstNode const* expression,
                               AstNode const* collection, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_REMOVE);
  node->reserve(4);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  node->addMember(options);
  node->addMember(collection);
  node->addMember(expression);
  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_OLD), false));

  return node;
}

/// @brief create an AST insert node
AstNode* Ast::createNodeInsert(AstNode const* expression,
                               AstNode const* collection, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_INSERT);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  bool returnOld = false;
  if (options->type == NODE_TYPE_OBJECT) {
    auto ops = ExecutionPlan::parseModificationOptions(options);
    returnOld = ops.isOverwriteModeUpdateReplace();
  }

  node->reserve(returnOld ? 5 : 4);
  node->addMember(options);
  node->addMember(collection);
  node->addMember(expression);
  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_NEW), false));
  if (returnOld) {
    node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_OLD), false));
  }

  return node;
}

/// @brief create an AST update node
AstNode* Ast::createNodeUpdate(AstNode const* keyExpression, AstNode const* docExpression,
                               AstNode const* collection, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_UPDATE);
  node->reserve(6);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  node->addMember(options);
  node->addMember(collection);
  node->addMember(docExpression);

  if (keyExpression != nullptr) {
    node->addMember(keyExpression);
  } else {
    node->addMember(&_specialNodes.NopNode);
  }

  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_OLD), false));
  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_NEW), false));

  return node;
}

/// @brief create an AST replace node
AstNode* Ast::createNodeReplace(AstNode const* keyExpression, AstNode const* docExpression,
                                AstNode const* collection, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_REPLACE);
  node->reserve(6);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  node->addMember(options);
  node->addMember(collection);
  node->addMember(docExpression);

  if (keyExpression != nullptr) {
    node->addMember(keyExpression);
  } else {
    node->addMember(&_specialNodes.NopNode);
  }

  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_OLD), false));
  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_NEW), false));

  return node;
}

/// @brief create an AST upsert node
AstNode* Ast::createNodeUpsert(AstNodeType type, AstNode const* docVariable,
                               AstNode const* insertExpression, AstNode const* updateExpression,
                               AstNode const* collection, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_UPSERT);
  node->reserve(7);

  node->setIntValue(static_cast<int64_t>(type));

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  node->addMember(options);
  node->addMember(collection);
  node->addMember(docVariable);
  node->addMember(insertExpression);
  node->addMember(updateExpression);

  node->addMember(createNodeReference(Variable::NAME_OLD));
  node->addMember(createNodeVariable(TRI_CHAR_LENGTH_PAIR(Variable::NAME_NEW), false));

  return node;
}

/// @brief create an AST distinct node
AstNode* Ast::createNodeDistinct(AstNode const* value) {
  AstNode* node = createNode(NODE_TYPE_DISTINCT);

  node->addMember(value);

  return node;
}

/// @brief create an AST collect node
AstNode* Ast::createNodeCollect(AstNode const* groups, AstNode const* aggregates,
                                AstNode const* into, AstNode const* intoExpression,
                                AstNode const* keepVariables, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_COLLECT);
  node->reserve(6);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  node->addMember(options);
  node->addMember(groups);  // may be an empty array

  // wrap aggregates again
  auto agg = createNode(NODE_TYPE_AGGREGATIONS);
  agg->addMember(aggregates);  // may be an empty array
  node->addMember(agg);

  node->addMember(into != nullptr ? into : &_specialNodes.NopNode);
  node->addMember(intoExpression != nullptr ? intoExpression : &_specialNodes.NopNode);
  node->addMember(keepVariables != nullptr ? keepVariables : &_specialNodes.NopNode);

  return node;
}

/// @brief create an AST collect node, COUNT INTO
AstNode* Ast::createNodeCollectCount(AstNode const* list, char const* name,
                                     size_t nameLength, AstNode const* options) {
  AstNode* node = createNode(NODE_TYPE_COLLECT_COUNT);
  node->reserve(2);

  if (options == nullptr) {
    // no options given. now use default options
    options = &_specialNodes.NopNode;
  }

  node->addMember(options);
  node->addMember(list);

  AstNode* variable = createNodeVariable(name, nameLength, true);
  node->addMember(variable);

  return node;
}

/// @brief create an AST sort node
AstNode* Ast::createNodeSort(AstNode const* list) {
  AstNode* node = createNode(NODE_TYPE_SORT);
  node->addMember(list);

  return node;
}

/// @brief create an AST sort element node
AstNode* Ast::createNodeSortElement(AstNode const* expression, AstNode const* ascending) {
  AstNode* node = createNode(NODE_TYPE_SORT_ELEMENT);
  node->reserve(2);
  node->addMember(expression);
  node->addMember(ascending);

  return node;
}

/// @brief create an AST limit node
AstNode* Ast::createNodeLimit(AstNode const* offset, AstNode const* count) {
  AstNode* node = createNode(NODE_TYPE_LIMIT);
  node->reserve(2);
  node->addMember(offset);
  node->addMember(count);

  return node;
}

/// @brief create an AST assign node, used in COLLECT statements
AstNode* Ast::createNodeAssign(char const* variableName, size_t nameLength,
                               AstNode const* expression) {
  if (variableName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_ASSIGN);
  node->reserve(2);
  AstNode* variable = createNodeVariable(variableName, nameLength, true);
  node->addMember(variable);
  node->addMember(expression);

  return node;
}

/// @brief create an AST variable node
AstNode* Ast::createNodeVariable(char const* name, size_t nameLength, bool isUserDefined) {
  if (name == nullptr || nameLength == 0) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (isUserDefined && *name == '_') {
    ::throwFormattedError(_query, TRI_ERROR_QUERY_VARIABLE_NAME_INVALID, name);
  }

  if (_scopes.existsVariable(name, nameLength)) {
    if (!isUserDefined && (strcmp(name, Variable::NAME_OLD) == 0 ||
                           strcmp(name, Variable::NAME_NEW) == 0)) {
      // special variable
      auto variable = _variables.createVariable(std::string(name, nameLength), isUserDefined);
      _scopes.replaceVariable(variable);

      AstNode* node = createNode(NODE_TYPE_VARIABLE);
      node->setData(static_cast<void*>(variable));

      return node;
    }

    ::throwFormattedError(_query, TRI_ERROR_QUERY_VARIABLE_REDECLARED, name);
  }

  auto variable = _variables.createVariable(std::string(name, nameLength), isUserDefined);
  _scopes.addVariable(variable);

  AstNode* node = createNode(NODE_TYPE_VARIABLE);
  node->setData(static_cast<void*>(variable));

  return node;
}

/// @brief create an AST datasource
/// this function will return either an AST collection or an AST view node
AstNode* Ast::createNodeDataSource(arangodb::CollectionNameResolver const& resolver,
                                   char const* name, size_t nameLength,
                                   AccessMode::Type accessType,
                                   bool validateName, bool failIfDoesNotExist) {
  arangodb::velocypack::StringRef nameRef(name, nameLength);

  // will throw if validation fails
  validateDataSourceName(nameRef, validateName);
  // this call may update nameRef
  LogicalCollection::Category const* category =
      injectDataSourceInQuery(*this, resolver, accessType, failIfDoesNotExist, nameRef);

  if (category == LogicalCollection::category()) {
    return createNodeCollectionNoValidation(nameRef, accessType);
  }
  if (category == LogicalView::category()) {
    AstNode* node = createNode(NODE_TYPE_VIEW);
    node->setStringValue(nameRef.data(), nameRef.size());
    return node;
  }
  // injectDataSourceInQuery is supposed to throw in this case.
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_INTERNAL,
      "invalid data source category in createNodeDataSource");
}

/// @brief create an AST collection node
AstNode* Ast::createNodeCollection(arangodb::CollectionNameResolver const& resolver,
                                   char const* name, size_t nameLength,
                                   AccessMode::Type accessType) {
  arangodb::velocypack::StringRef nameRef(name, nameLength);

  // will throw if validation fails
  validateDataSourceName(nameRef, true);
  // this call may update nameRef
  LogicalCollection::Category const* category =
      injectDataSourceInQuery(*this, resolver, accessType, false, nameRef);

  if (category == LogicalCollection::category()) {
    // add collection to query
    _query.collections().add(nameRef.toString(), accessType, Collection::Hint::Collection);

    // call private function after validation
    return createNodeCollectionNoValidation(nameRef, accessType);
  }
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_MISMATCH,
                                 nameRef.toString() +
                                     " is required to be a collection.");
}

/// @brief create an AST reference node
AstNode* Ast::createNodeReference(char const* variableName, size_t nameLength) {
  if (variableName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  return createNodeReference(std::string(variableName, nameLength));
}

/// @brief create an AST reference node
AstNode* Ast::createNodeReference(std::string const& variableName) {
  AstNode* node = createNode(NODE_TYPE_REFERENCE);

  auto variable = _scopes.getVariable(variableName);

  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "variable not found in reference AstNode");
  }

  node->setData(variable);

  return node;
}

/// @brief create an AST reference node
AstNode* Ast::createNodeReference(Variable const* variable) {
  AstNode* node = createNode(NODE_TYPE_REFERENCE);
  node->setData(variable);

  return node;
}

/// @brief create an AST subquery reference node
AstNode* Ast::createNodeSubqueryReference(std::string const& variableName) {
  AstNode* node = createNode(NODE_TYPE_REFERENCE);
  node->setFlag(AstNodeFlagType::FLAG_SUBQUERY_REFERENCE);

  auto variable = _scopes.getVariable(variableName);

  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "variable not found in reference AstNode");
  }

  node->setData(variable);

  return node;
}

/// @brief create an AST variable access
AstNode* Ast::createNodeAccess(Variable const* variable,
                               std::vector<basics::AttributeName> const& field) {
  TRI_ASSERT(!field.empty());
  AstNode* node = createNodeReference(variable);
  for (auto const& it : field) {
    node = createNodeAttributeAccess(node, it.name.data(), it.name.size());
  }
  return node;
}

AstNode* Ast::createNodeAttributeAccess(AstNode const* refNode,
                                        std::vector<std::string> const& path) {
  AstNode* rv = refNode->clone(this);
  for (auto const& part : path) {
    char const* p = _resources.registerString(part.data(), part.size());
    rv = createNodeAttributeAccess(rv, p, part.size());
  }
  return rv;
}

/// @brief create an AST parameter node
AstNode* Ast::createNodeParameter(char const* name, size_t length) {
  if (name == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_PARAMETER);
  node->setStringValue(name, length);

  // insert bind parameter name into list of found parameters
  _bindParameters.emplace(name, length);
  _containsBindParameters = true;

  return node;
}

AstNode* Ast::createNodeParameterDatasource(char const* name, size_t length) {
  if (name == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_PARAMETER_DATASOURCE);
  node->setStringValue(name, length);

  // insert bind parameter name into list of found parameters
  _bindParameters.emplace(name, length);
  _containsBindParameters = true;

  return node;
}

/// @brief create an AST quantifier node
AstNode* Ast::createNodeQuantifier(int64_t type) {
  AstNode* node = createNode(NODE_TYPE_QUANTIFIER);
  node->setIntValue(type);

  return node;
}

/// @brief create an AST unary operator node
AstNode* Ast::createNodeUnaryOperator(AstNodeType type, AstNode const* operand) {
  AstNode* node = createNode(type);
  node->addMember(operand);

  return node;
}

/// @brief create an AST binary operator node
AstNode* Ast::createNodeBinaryOperator(AstNodeType type, AstNode const* lhs,
                                       AstNode const* rhs) {
  // do a bit of normalization here, so that attribute accesses are normally
  // on the left side of a comparison. this may allow future simplifications
  // of code that check filter conditions
  // note that there will still be cases in which both sides of the comparison
  // contain an attribute access, e.g.  doc.value1 == doc.value2
  bool swap = false;
  if (type == NODE_TYPE_OPERATOR_BINARY_EQ && rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS &&
      lhs->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    // value == doc.value  =>  doc.value == value
    swap = true;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_NE && rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS &&
             lhs->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    // value != doc.value  =>  doc.value != value
    swap = true;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_GT && rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS &&
             lhs->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    // value > doc.value  =>  doc.value < value
    type = NODE_TYPE_OPERATOR_BINARY_LT;
    swap = true;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_LT && rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS &&
             lhs->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    // value < doc.value  =>  doc.value > value
    type = NODE_TYPE_OPERATOR_BINARY_GT;
    swap = true;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_GE && rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS &&
             lhs->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    // value >= doc.value  =>  doc.value <= value
    type = NODE_TYPE_OPERATOR_BINARY_LE;
    swap = true;
  } else if (type == NODE_TYPE_OPERATOR_BINARY_LE && rhs->type == NODE_TYPE_ATTRIBUTE_ACCESS &&
             lhs->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
    // value <= doc.value  =>  doc.value >= value
    type = NODE_TYPE_OPERATOR_BINARY_GE;
    swap = true;
  }

  AstNode* node = createNode(type);
  node->reserve(2);

  node->addMember(swap ? rhs : lhs);
  node->addMember(swap ? lhs : rhs);

  // initialize sortedness information (currently used for the IN/NOT IN
  // operators only) for nodes of type ==, < or <=, the bool means if the range
  // definitely excludes the "null" value the default value for this is false.
  node->setBoolValue(false);

  return node;
}

/// @brief create an AST binary array operator node
AstNode* Ast::createNodeBinaryArrayOperator(AstNodeType type, AstNode const* lhs,
                                            AstNode const* rhs, AstNode const* quantifier) {
  // re-use existing function
  AstNode* node = createNodeBinaryOperator(type, lhs, rhs);
  node->addMember(quantifier);

  TRI_ASSERT(node->isArrayComparisonOperator());
  TRI_ASSERT(node->numMembers() == 3);

  return node;
}

/// @brief create an AST ternary operator node, using the condition as the truth part
AstNode* Ast::createNodeTernaryOperator(AstNode const* condition, 
                                        AstNode const* falsePart) {
  AstNode* node = createNode(NODE_TYPE_OPERATOR_TERNARY);
  node->reserve(2);
  node->addMember(condition);
  node->addMember(falsePart);

  return node;
}

/// @brief create an AST ternary operator node
AstNode* Ast::createNodeTernaryOperator(AstNode const* condition, AstNode const* truePart,
                                        AstNode const* falsePart) {
  AstNode* node = createNode(NODE_TYPE_OPERATOR_TERNARY);
  node->reserve(3);
  node->addMember(condition);
  node->addMember(truePart);
  node->addMember(falsePart);

  return node;
}

/// @brief create an AST attribute access node
/// note that the caller must make sure that char* data remains valid!
AstNode* Ast::createNodeAttributeAccess(AstNode const* accessed,
                                        char const* attributeName, size_t nameLength) {
  if (attributeName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_ATTRIBUTE_ACCESS);
  node->addMember(accessed);
  node->setStringValue(attributeName, nameLength);

  return node;
}

/// @brief create an AST attribute access node w/ bind parameter
AstNode* Ast::createNodeBoundAttributeAccess(AstNode const* accessed, AstNode const* parameter) {
  AstNode* node = createNode(NODE_TYPE_BOUND_ATTRIBUTE_ACCESS);
  node->reserve(2);
  node->setStringValue(parameter->getStringValue(), parameter->getStringLength());
  node->addMember(accessed);
  node->addMember(parameter);

  _containsBindParameters = true;

  return node;
}

/// @brief create an AST indexed access node
AstNode* Ast::createNodeIndexedAccess(AstNode const* accessed, AstNode const* indexValue) {
  AstNode* node = createNode(NODE_TYPE_INDEXED_ACCESS);
  node->reserve(2);
  node->addMember(accessed);
  node->addMember(indexValue);

  return node;
}

/// @brief create an AST array limit node (offset, count)
AstNode* Ast::createNodeArrayLimit(AstNode const* offset, AstNode const* count) {
  AstNode* node = createNode(NODE_TYPE_ARRAY_LIMIT);
  node->reserve(2);

  if (offset == nullptr) {
    offset = createNodeValueInt(0);
  }
  node->addMember(offset);
  node->addMember(count);

  return node;
}

/// @brief create an AST expansion node, with or without a filter
AstNode* Ast::createNodeExpansion(int64_t levels, AstNode const* iterator,
                                  AstNode const* expanded, AstNode const* filter,
                                  AstNode const* limit, AstNode const* projection) {
  AstNode* node = createNode(NODE_TYPE_EXPANSION);
  node->reserve(5);
  node->setIntValue(levels);

  node->addMember(iterator);
  node->addMember(expanded);

  if (filter == nullptr) {
    node->addMember(createNodeNop());
  } else {
    node->addMember(filter);
  }

  if (limit == nullptr) {
    node->addMember(createNodeNop());
  } else {
    TRI_ASSERT(limit->type == NODE_TYPE_ARRAY_LIMIT);
    node->addMember(limit);
  }

  if (projection == nullptr) {
    node->addMember(createNodeNop());
  } else {
    node->addMember(projection);
  }

  TRI_ASSERT(node->numMembers() == 5);

  return node;
}

/// @brief create an AST iterator node
AstNode* Ast::createNodeIterator(char const* variableName, size_t nameLength,
                                 AstNode const* expanded) {
  if (variableName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_ITERATOR);
  node->reserve(2);

  AstNode* variable = createNodeVariable(variableName, nameLength, false);
  node->addMember(variable);
  node->addMember(expanded);

  return node;
}

/// @brief create an AST null value node
AstNode* Ast::createNodeValueNull() {
  // performance optimization:
  // return a pointer to the singleton null node
  // note: this node is never registered nor freed
  return const_cast<AstNode*>(&_specialNodes.NullNode);
}

/// @brief create an AST bool value node
AstNode* Ast::createNodeValueBool(bool value) {
  // performance optimization:
  // return a pointer to the singleton bool nodes
  // note: these nodes are never registered nor freed
  if (value) {
    return const_cast<AstNode*>(&_specialNodes.TrueNode);
  }

  return const_cast<AstNode*>(&_specialNodes.FalseNode);
}

/// @brief create an AST int value node
AstNode* Ast::createNodeValueInt(int64_t value) {
  if (value == 0) {
    // performance optimization:
    // return a pointer to the singleton zero node
    // note: these nodes are never registered nor freed
    return const_cast<AstNode*>(&_specialNodes.ZeroNode);
  }

  AstNode* node = createNode(NODE_TYPE_VALUE);
  node->setValueType(VALUE_TYPE_INT);
  node->setIntValue(value);
  node->setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
  node->setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
  node->setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);

  return node;
}

/// @brief create an AST double value node
AstNode* Ast::createNodeValueDouble(double value) {
  AstNode* node = createNode(NODE_TYPE_VALUE);
  node->setValueType(VALUE_TYPE_DOUBLE);
  node->setDoubleValue(value);
  node->setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
  node->setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
  node->setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);

  return node;
}

/// @brief create an AST string value node
AstNode* Ast::createNodeValueString(char const* value, size_t length) {
  if (value == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (length == 0) {
    // performance optimization:
    // return a pointer to the singleton empty string node
    // note: these nodes are never registered nor freed
    return const_cast<AstNode*>(&_specialNodes.EmptyStringNode);
  }

  AstNode* node = createNode(NODE_TYPE_VALUE);
  node->setValueType(VALUE_TYPE_STRING);
  node->setStringValue(value, length);
  node->setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
  node->setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
  node->setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);

  return node;
}

/// @brief create an AST array node
AstNode* Ast::createNodeArray() { return createNode(NODE_TYPE_ARRAY); }

/// @brief create an AST array node
AstNode* Ast::createNodeArray(size_t size) {
  AstNode* node = createNodeArray();

  TRI_ASSERT(node != nullptr);
  if (size > 0) {
    node->reserve(size);
  }

  return node;
}

/// @brief create an AST unique array node, AND-merged from two other arrays
/// the resulting array has no particular order
AstNode* Ast::createNodeIntersectedArray(AstNode const* lhs, AstNode const* rhs) {
  TRI_ASSERT(lhs->isArray() && lhs->isConstant());
  TRI_ASSERT(rhs->isArray() && rhs->isConstant());

  size_t nl = lhs->numMembers();
  size_t nr = rhs->numMembers();

  if (nl > nr) {
    // we want lhs to be the shorter of the two arrays, so we use less
    // memory for the lookup cache
    std::swap(lhs, rhs);
    std::swap(nl, nr);
  }

  std::unordered_map<VPackSlice, AstNode const*, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      cache(nl, arangodb::basics::VelocyPackHelper::VPackHash(),
            arangodb::basics::VelocyPackHelper::VPackEqual());

  VPackBuilder temp;

  for (size_t i = 0; i < nl; ++i) {
    auto member = lhs->getMemberUnchecked(i);
    VPackSlice slice = member->computeValue(&temp);

    cache.try_emplace(slice, member);
  }

  auto node = createNodeArray(nl);

  for (size_t i = 0; i < nr; ++i) {
    auto member = rhs->getMemberUnchecked(i);
    VPackSlice slice = member->computeValue(&temp);

    auto it = cache.find(slice);

    if (it != cache.end() &&
        (*it).second != nullptr) {
      // add to output
      node->addMember((*it).second);
      // make sure we don't add the same value again
      (*it).second = nullptr;
    }
  }

  return node;
}

/// @brief create an AST unique array node, OR-merged from two other arrays
/// the resulting array will be sorted by value
AstNode* Ast::createNodeUnionizedArray(AstNode const* lhs, AstNode const* rhs) {
  TRI_ASSERT(lhs->isArray() && lhs->isConstant());
  TRI_ASSERT(rhs->isArray() && rhs->isConstant());

  size_t const nl = lhs->numMembers();
  size_t const nr = rhs->numMembers();

  auto node = createNodeArray(nl + nr);

  std::unordered_map<VPackSlice, AstNode const*, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      cache(nl + nr, arangodb::basics::VelocyPackHelper::VPackHash(),
            arangodb::basics::VelocyPackHelper::VPackEqual());

  VPackBuilder temp;

  for (size_t i = 0; i < nl + nr; ++i) {
    AstNode* member;
    if (i < nl) {
      member = lhs->getMemberUnchecked(i);
    } else {
      member = rhs->getMemberUnchecked(i - nl);
    }
    VPackSlice slice = member->computeValue(&temp);

    if (cache.try_emplace(slice, member).second) {
      // only insert unique values
      node->addMember(member);
    }
  }

  node->sort();

  return node;
}

/// @brief create an AST object node
AstNode* Ast::createNodeObject() { return createNode(NODE_TYPE_OBJECT); }

/// @brief create an AST object element node
AstNode* Ast::createNodeObjectElement(char const* attributeName, size_t nameLength,
                                      AstNode const* expression) {
  if (attributeName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  AstNode* node = createNode(NODE_TYPE_OBJECT_ELEMENT);
  node->setStringValue(attributeName, nameLength);
  node->addMember(expression);

  return node;
}

/// @brief create an AST calculated object element node
AstNode* Ast::createNodeCalculatedObjectElement(AstNode const* attributeName,
                                                AstNode const* expression) {
  AstNode* node = createNode(NODE_TYPE_CALCULATED_OBJECT_ELEMENT);
  node->reserve(2);
  node->addMember(attributeName);
  node->addMember(expression);

  return node;
}

/// @brief create an AST with collections node
AstNode* Ast::createNodeWithCollections(AstNode const* collections,
                                        arangodb::CollectionNameResolver const& resolver) {
  AstNode* node = createNode(NODE_TYPE_COLLECTION_LIST);

  TRI_ASSERT(collections->type == NODE_TYPE_ARRAY);

  for (size_t i = 0; i < collections->numMembers(); ++i) {
    auto c = collections->getMember(i);

    if (c->isStringValue()) {
      std::string const name = c->getString();
      // this call may update nameRef
      arangodb::velocypack::StringRef nameRef(name);
      LogicalDataSource::Category const* category =
          injectDataSourceInQuery(*this, resolver, AccessMode::Type::READ, false, nameRef);
      if (category == LogicalCollection::category()) {
        _query.collections().add(name, AccessMode::Type::READ, Collection::Hint::Collection);

        if (ServerState::instance()->isCoordinator()) {
          auto& ci = _query.vocbase().server().getFeature<ClusterFeature>().clusterInfo();

          // We want to tolerate that a collection name is given here
          // which does not exist, if only for some unit tests:
          auto coll = ci.getCollectionNT(_query.vocbase().name(), name);
          if (coll != nullptr) {
            auto names = coll->realNames();

            for (auto const& n : names) {
              arangodb::velocypack::StringRef shardsNameRef(n);
              LogicalDataSource::Category const* shardsCategory =
                  injectDataSourceInQuery(*this, resolver, AccessMode::Type::READ,
                                          false, shardsNameRef);
              TRI_ASSERT(shardsCategory == LogicalCollection::category());
            }
          }
        }
      }
    }  // else bindParameter use default for collection bindVar

    // We do not need to propagate these members
    node->addMember(c);
  }

  AstNode* with = createNode(NODE_TYPE_WITH);

  with->addMember(node);

  return with;
}

/// @brief create an AST collection list node
AstNode* Ast::createNodeCollectionList(AstNode const* edgeCollections,
                                       CollectionNameResolver const& resolver) {
  AstNode* node = createNode(NODE_TYPE_COLLECTION_LIST);

  TRI_ASSERT(edgeCollections->type == NODE_TYPE_ARRAY);

  auto ss = ServerState::instance();
  auto doTheAdd = [&](std::string const& name) {
    arangodb::velocypack::StringRef nameRef(name);
    LogicalDataSource::Category const* category =
        injectDataSourceInQuery(*this, resolver, AccessMode::Type::READ, false, nameRef);
    if (category == LogicalCollection::category()) {
      if (ss->isCoordinator()) {
        auto& ci = _query.vocbase().server().getFeature<ClusterFeature>().clusterInfo();
        auto c = ci.getCollectionNT(_query.vocbase().name(), name);
        if (c != nullptr) {
          auto const& names = c->realNames();

          for (auto const& n : names) {
            arangodb::velocypack::StringRef shardsNameRef(n);
            LogicalDataSource::Category const* shardsCategory =
                injectDataSourceInQuery(*this, resolver, AccessMode::Type::READ,
                                        false, shardsNameRef);
            TRI_ASSERT(shardsCategory == LogicalCollection::category());
          }
        }  // else { TODO Should we really not react? }
      }
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_MISMATCH,
                                     nameRef.toString() +
                                         " is required to be a collection.");
    }
  };

  for (size_t i = 0; i < edgeCollections->numMembers(); ++i) {
    // TODO Direction Parsing!
    auto eC = edgeCollections->getMember(i);

    if (eC->isStringValue()) {
      doTheAdd(eC->getString());
    } else if (eC->type == NODE_TYPE_DIRECTION) {
      TRI_ASSERT(eC->numMembers() == 2);
      auto eCSub = eC->getMember(1);

      if (eCSub->isStringValue()) {
        doTheAdd(eCSub->getString());
      }
    }  // else bindParameter use default for collection bindVar

    // We do not need to propagate these members
    node->addMember(eC);
  }

  return node;
}

/// @brief create an AST direction node
AstNode* Ast::createNodeDirection(uint64_t direction, uint64_t steps) {
  AstNode* node = createNode(NODE_TYPE_DIRECTION);
  node->reserve(2);

  AstNode* dir = createNodeValueInt(direction);
  AstNode* step = createNodeValueInt(steps);
  node->addMember(dir);
  node->addMember(step);

  TRI_ASSERT(node->numMembers() == 2);
  return node;
}

AstNode* Ast::createNodeDirection(uint64_t direction, AstNode const* steps) {
  AstNode* node = createNode(NODE_TYPE_DIRECTION);
  node->reserve(2);
  AstNode* dir = createNodeValueInt(direction);

  node->addMember(dir);
  node->addMember(steps);

  TRI_ASSERT(node->numMembers() == 2);
  return node;
}

AstNode* Ast::createNodeCollectionDirection(uint64_t direction, AstNode const* collection) {
  AstNode* node = createNode(NODE_TYPE_DIRECTION);
  node->reserve(2);
  AstNode* dir = createNodeValueInt(direction);

  node->addMember(dir);
  node->addMember(collection);

  TRI_ASSERT(node->numMembers() == 2);
  return node;
}

/// @brief create an AST traversal node
AstNode* Ast::createNodeTraversal(AstNode const* outVars, AstNode const* graphInfo) {
  TRI_ASSERT(outVars->type == NODE_TYPE_ARRAY);
  TRI_ASSERT(graphInfo->type == NODE_TYPE_ARRAY);
  AstNode* node = createNode(NODE_TYPE_TRAVERSAL);
  node->reserve(outVars->numMembers() + graphInfo->numMembers());

  TRI_ASSERT(graphInfo->numMembers() == 5);
  TRI_ASSERT(outVars->numMembers() > 0);
  TRI_ASSERT(outVars->numMembers() < 4);

  // Add GraphInfo
  for (size_t i = 0; i < graphInfo->numMembers(); ++i) {
    node->addMember(graphInfo->getMemberUnchecked(i));
  }

  // Add variables
  for (size_t i = 0; i < outVars->numMembers(); ++i) {
    node->addMember(outVars->getMemberUnchecked(i));
  }
  TRI_ASSERT(node->numMembers() == graphInfo->numMembers() + outVars->numMembers());

  _containsTraversal = true;

  return node;
}

/// @brief create an AST shortest path node
AstNode* Ast::createNodeShortestPath(AstNode const* outVars, AstNode const* graphInfo) {
  TRI_ASSERT(outVars->type == NODE_TYPE_ARRAY);
  TRI_ASSERT(graphInfo->type == NODE_TYPE_ARRAY);
  AstNode* node = createNode(NODE_TYPE_SHORTEST_PATH);
  node->reserve(outVars->numMembers() + graphInfo->numMembers());

  TRI_ASSERT(graphInfo->numMembers() == 5);
  TRI_ASSERT(outVars->numMembers() > 0);
  TRI_ASSERT(outVars->numMembers() < 3);

  // Add GraphInfo
  for (size_t i = 0; i < graphInfo->numMembers(); ++i) {
    node->addMember(graphInfo->getMemberUnchecked(i));
  }

  // Add variables
  for (size_t i = 0; i < outVars->numMembers(); ++i) {
    node->addMember(outVars->getMemberUnchecked(i));
  }
  TRI_ASSERT(node->numMembers() == graphInfo->numMembers() + outVars->numMembers());

  _containsTraversal = true;

  return node;
}

/// @brief create an AST k-shortest paths node
AstNode* Ast::createNodeKShortestPaths(AstNode const* outVars, AstNode const* graphInfo) {
  TRI_ASSERT(outVars->type == NODE_TYPE_ARRAY);
  TRI_ASSERT(graphInfo->type == NODE_TYPE_ARRAY);
  AstNode* node = createNode(NODE_TYPE_K_SHORTEST_PATHS);
  node->reserve(outVars->numMembers() + graphInfo->numMembers());

  TRI_ASSERT(graphInfo->numMembers() == 5);
  TRI_ASSERT(outVars->numMembers() > 0);
  TRI_ASSERT(outVars->numMembers() < 3);

  // Add GraphInfo
  for (size_t i = 0; i < graphInfo->numMembers(); ++i) {
    node->addMember(graphInfo->getMemberUnchecked(i));
  }

  // Add variables
  for (size_t i = 0; i < outVars->numMembers(); ++i) {
    node->addMember(outVars->getMemberUnchecked(i));
  }
  TRI_ASSERT(node->numMembers() == graphInfo->numMembers() + outVars->numMembers());

  _containsTraversal = true;

  return node;
}

AstNode const* Ast::createNodeOptions(AstNode const* options) const {
  if (options != nullptr) {
    return options;
  }
  return &_specialNodes.NopNode;
}

/// @brief create an AST function call node
AstNode* Ast::createNodeFunctionCall(char const* functionName, size_t length,
                                     AstNode const* arguments) {
  if (functionName == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  auto normalized = normalizeFunctionName(functionName, length);

  AstNode* node;

  if (normalized.second) {
    // built-in function
    auto func = AqlFunctionFeature::getFunctionByName(normalized.first);
    TRI_ASSERT(func != nullptr);
   
    node = createNode(NODE_TYPE_FCALL);
    // register a pointer to the function
    node->setData(static_cast<void const*>(func));

    TRI_ASSERT(arguments != nullptr);
    TRI_ASSERT(arguments->type == NODE_TYPE_ARRAY);

    // validate number of function call arguments
    size_t const n = arguments->numMembers();

    auto numExpectedArguments = func->numArguments();

    if (n < numExpectedArguments.first || n > numExpectedArguments.second) {
      std::string const fname(functionName, length);

      THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_FUNCTION_ARGUMENT_NUMBER_MISMATCH,
                                    fname.c_str(),
                                    static_cast<int>(numExpectedArguments.first),
                                    static_cast<int>(numExpectedArguments.second));
    }

    if (!func->hasFlag(Function::Flags::CanRunOnDBServer)) {
      // this also qualifies a query for potentially reading or modifying
      // documents via function calls!
      _functionsMayAccessDocuments = true;
    }
  } else {
    // user-defined function
    node = createNode(NODE_TYPE_FCALL_USER);
    // register the function name
    char* fname = _resources.registerString(normalized.first);
    node->setStringValue(fname, normalized.first.size());

    _functionsMayAccessDocuments = true;
  }

  node->addMember(arguments);

  return node;
}

/// @brief create an AST range node
AstNode* Ast::createNodeRange(AstNode const* start, AstNode const* end) {
  AstNode* node = createNode(NODE_TYPE_RANGE);
  node->reserve(2);
  node->addMember(start);
  node->addMember(end);

  return node;
}

/// @brief create an AST nop node
AstNode* Ast::createNodeNop() { return const_cast<AstNode*>(&_specialNodes.NopNode); }

/// @brief create an AST n-ary operator node
AstNode* Ast::createNodeNaryOperator(AstNodeType type) {
  TRI_ASSERT(type == NODE_TYPE_OPERATOR_NARY_AND || type == NODE_TYPE_OPERATOR_NARY_OR);

  return createNode(type);
}

/// @brief create an AST n-ary operator node
AstNode* Ast::createNodeNaryOperator(AstNodeType type, AstNode const* child) {
  AstNode* node = createNodeNaryOperator(type);
  node->addMember(child);

  return node;
}

/// @brief injects bind parameters into the AST
void Ast::injectBindParameters(BindParameters& parameters,
                               arangodb::CollectionNameResolver const& resolver) {
  auto& p = parameters.get();

  if (_containsBindParameters || _containsTraversal) {
    // inject bind parameters into query AST
    auto func = [&](AstNode* node) -> AstNode* {
      if (node->type == NODE_TYPE_PARAMETER || node->type == NODE_TYPE_PARAMETER_DATASOURCE) {
        // found a bind parameter in the query string
        std::string const param = node->getString();

        if (param.empty()) {
          // parameter name must not be empty
          ::throwFormattedError(_query, TRI_ERROR_QUERY_BIND_PARAMETER_MISSING, param.c_str());
        }

        auto const& it = p.find(param);

        if (it == p.end()) {
          // query uses a bind parameter that was not defined by the user
          ::throwFormattedError(_query, TRI_ERROR_QUERY_BIND_PARAMETER_MISSING, param.c_str());
        }

        // mark the bind parameter as being used
        (*it).second.second = true;

        auto const& value = (*it).second.first;

        if (node->type == NODE_TYPE_PARAMETER) {
          // bind parameter containing a value literal
          node = nodeFromVPack(value, true);

          if (node != nullptr) {
            // already mark node as constant here
            node->setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
            // mark node as simple
            node->setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
            // mark node as executable on db-server
            node->setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);
            // mark node as deterministic
            node->setFlag(DETERMINED_NONDETERMINISTIC);

            // finally note that the node was created from a bind parameter
            node->setFlag(FLAG_BIND_PARAMETER);
          }
        } else {
          TRI_ASSERT(node->type == NODE_TYPE_PARAMETER_DATASOURCE);
         
          if (!value.isString()) {
            // we can get here in case `WITH @col ...` when the value of @col
            // is not a string
            ::throwFormattedError(_query, TRI_ERROR_QUERY_BIND_PARAMETER_TYPE, param.c_str());
            // query will have been aborted here
          }

          // bound data source parameter
          TRI_ASSERT(value.isString());
          VPackValueLength l;
          char const* name = value.getString(l);

          // check if the collection was used in a data-modification query
          bool isWriteCollection = false;

          arangodb::velocypack::StringRef paramRef(param);
          arangodb::velocypack::StringRef nameRef(name, l);

          AstNode* newNode = nullptr;
          for (auto const& it : _writeCollections) {
            auto const& c = it.first;

            if (c->type == NODE_TYPE_PARAMETER_DATASOURCE &&
                paramRef == arangodb::velocypack::StringRef(c->getStringValue(),
                                                            c->getStringLength())) {
              // bind parameter still present in _writeCollections
              TRI_ASSERT(newNode == nullptr);
              isWriteCollection = true;
              break;
            } else if (c->type == NODE_TYPE_COLLECTION &&
                nameRef == arangodb::velocypack::StringRef(c->getStringValue(),
                                                           c->getStringLength())) {
              // bind parameter was already replaced with a proper collection node 
              // in _writeCollections
              TRI_ASSERT(newNode == nullptr);
              isWriteCollection = true;
              newNode = const_cast<AstNode*>(c);
              break;
            }
          }

          TRI_ASSERT(newNode == nullptr || isWriteCollection);

          if (newNode == nullptr) {
            newNode = createNodeDataSource(resolver, name, l,
                                           isWriteCollection ? AccessMode::Type::WRITE
                                                             : AccessMode::Type::READ,
                                        false, true);
            TRI_ASSERT(newNode != nullptr);

            if (isWriteCollection) {
              // must update AST info now for all nodes that contained this
              // parameter
              for (auto& it : _writeCollections) {
                auto& c = it.first;

                if (c->type == NODE_TYPE_PARAMETER_DATASOURCE &&
                    paramRef == arangodb::velocypack::StringRef(c->getStringValue(),
                                                                c->getStringLength())) {
                  c = newNode;
                  // no break here. replace all occurrences
                }
              }
            }
          }
          node = newNode;
        }
      } else if (node->type == NODE_TYPE_BOUND_ATTRIBUTE_ACCESS) {
        // look at second sub-node. this is the (replaced) bind parameter
        auto name = node->getMember(1);

        if (name->type == NODE_TYPE_VALUE) {
          if (name->value.type == VALUE_TYPE_STRING && name->value.length != 0) {
            // convert into a regular attribute access node to simplify handling
            // later
            return createNodeAttributeAccess(node->getMember(0), name->getStringValue(),
                                             name->getStringLength());
          }
        } else if (name->type == NODE_TYPE_ARRAY) {
          // bind parameter is an array (e.g. ["a", "b", "c"]. now build the
          // attribute accesses for the array members recursively
          size_t const n = name->numMembers();

          AstNode* result = nullptr;
          if (n > 0) {
            result = node->getMember(0);
          }

          for (size_t i = 0; i < n; ++i) {
            auto part = name->getMember(i);
            if (part->value.type != VALUE_TYPE_STRING || part->value.length == 0) {
              // invalid attribute name part
              result = nullptr;
              break;
            }

            result = createNodeAttributeAccess(result, part->getStringValue(),
                                               part->getStringLength());
          }

          if (result != nullptr) {
            return result;
          }
        }
        // fallthrough to exception

        // if no string value was inserted for the parameter name, this is an
        // error
        THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_BIND_PARAMETER_TYPE,
                                      node->getString().c_str());
      } else if (node->type == NODE_TYPE_TRAVERSAL) {
        extractCollectionsFromGraph(node->getMember(2));
      } else if (node->type == NODE_TYPE_SHORTEST_PATH ||
                 node->type == NODE_TYPE_K_SHORTEST_PATHS) {
        extractCollectionsFromGraph(node->getMember(3));
      }

      return node;
    };

    _root = traverseAndModify(_root, func);
  }

  // add all collections used in data-modification statements
  for (auto& it : _writeCollections) {
    auto const& c = it.first;
    bool isExclusive = it.second;
    if (c->type == NODE_TYPE_COLLECTION) {
      std::string const name = c->getString();
      _query.collections().add(name, isExclusive ? AccessMode::Type::EXCLUSIVE
                                              : AccessMode::Type::WRITE, Collection::Hint::Collection);
      if (ServerState::instance()->isCoordinator()) {
        auto& ci = _query.vocbase().server().getFeature<ClusterFeature>().clusterInfo();

        // We want to tolerate that a collection name is given here
        // which does not exist, if only for some unit tests:
        auto coll = ci.getCollectionNT(_query.vocbase().name(), name);
        if (coll != nullptr) {
          auto names = coll->realNames();

          for (auto const& n : names) {
            _query.collections().add(n, isExclusive ? AccessMode::Type::EXCLUSIVE
                                                 : AccessMode::Type::WRITE, Collection::Hint::Collection);
          }
        }
      }
    }
  }

  for (auto it = p.begin(); it != p.end(); ++it) {
    if (!(*it).second.second) {
      THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_BIND_PARAMETER_UNDECLARED,
                                    (*it).first.c_str());
    }
  }
}

/// @brief replace an attribute access with just the variable
AstNode* Ast::replaceAttributeAccess(AstNode* node, Variable const* variable,
                                     std::vector<std::string> const& attribute) {
  TRI_ASSERT(!attribute.empty());
  if (attribute.empty()) {
    return node;
  }

  std::vector<arangodb::velocypack::StringRef> attributePath;

  auto visitor = [&](AstNode* node) -> AstNode* {
    if (node == nullptr) {
      return nullptr;
    }

    if (node->type != NODE_TYPE_ATTRIBUTE_ACCESS) {
      return node;
    }

    attributePath.clear();
    AstNode* origNode = node;

    while (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      attributePath.emplace_back(node->getStringValue(), node->getStringLength());
      node = node->getMember(0);
    }

    if (attributePath.size() != attribute.size()) {
      // different attribute
      return origNode;
    }
    for (size_t i = 0; i < attribute.size(); ++i) {
      if (attributePath[i] != attribute[i]) {
        // different attribute
        return origNode;
      }
    }
    // same attribute

    if (node->type == NODE_TYPE_REFERENCE) {
      auto v = static_cast<Variable*>(node->getData());
      if (v != nullptr && v->id == variable->id) {
        // our variable... now replace the attribute access with just the
        // variable
        return node;
      }
    }

    return origNode;
  };

  return traverseAndModify(node, visitor);
}

/// @brief replace variables
/*static*/ AstNode* Ast::replaceVariables(AstNode* node,
                               std::unordered_map<VariableId, Variable const*> const& replacements) {
  auto visitor = [&replacements](AstNode* node) -> AstNode* {
    if (node == nullptr) {
      return nullptr;
    }

    // reference to a variable
    if (node->type == NODE_TYPE_REFERENCE) {
      auto variable = static_cast<Variable*>(node->getData());

      if (variable != nullptr) {
        auto it = replacements.find(variable->id);

        if (it != replacements.end()) {
          // overwrite the node in place
          node->setData((*it).second);
        }
      }
      // intentionally falls through
    }

    return node;
  };

  return traverseAndModify(node, visitor);
}

/// @brief replace a variable reference in the expression with another
/// expression (e.g. inserting c = `a + b` into expression `c + 1` so the latter
/// becomes `a + b + 1`
/*static*/ AstNode* Ast::replaceVariableReference(AstNode* node, Variable const* variable,
                                       AstNode const* expressionNode) {
  struct SearchPattern {
    Variable const* variable;
    AstNode const* expressionNode;
  };

  SearchPattern const pattern{variable, expressionNode};

  auto visitor = [&pattern](AstNode* node) -> AstNode* {
    if (node == nullptr) {
      return nullptr;
    }

    // reference to a variable
    if (node->type == NODE_TYPE_REFERENCE &&
        static_cast<Variable const*>(node->getData()) == pattern.variable) {
      // found the target node. now insert the new node
      return const_cast<AstNode*>(pattern.expressionNode);
    }

    return node;
  };

  return traverseAndModify(node, visitor);
}

size_t Ast::validatedParallelism(AstNode const* value) {
  TRI_ASSERT(value != nullptr);
  TRI_ASSERT(value->isConstant());
  if (value->isIntValue()) {
    int64_t p = value->getIntValue();
    if (p > 0) {
      return static_cast<size_t>(p);
    }
  }
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, "parallelism: invalid value");
}

size_t Ast::extractParallelism(AstNode const* optionsNode) {
  if (optionsNode != nullptr && optionsNode->type == NODE_TYPE_OBJECT) {
    size_t n = optionsNode->numMembers();

    for (size_t i = 0; i < n; ++i) {
      auto member = optionsNode->getMemberUnchecked(i);

      if (member == nullptr || member->type != NODE_TYPE_OBJECT_ELEMENT) {
        continue;
      }
      auto const name = member->getStringRef();
      auto value = member->getMember(0);
      TRI_ASSERT(value->isConstant());

      if (name == "parallelism") {
        return validatedParallelism(value);
      }
    }
  }
  // default value
  return 1;
}

/// @brief optimizes the AST
/// this does not only optimize but also performs a few validations after
/// bind parameter injection. merging this pass with the regular AST
/// optimizations saves one extra pass over the AST
void Ast::validateAndOptimize(transaction::Methods& trx) {
  struct TraversalContext {
    TraversalContext(transaction::Methods& t) : trx(t) {}
    
    std::unordered_set<std::string> writeCollectionsSeen;
    std::unordered_map<std::string, int64_t> collectionsFirstSeen;
    std::unordered_map<Variable const*, AstNode const*> variableDefinitions;
    AqlFunctionsInternalCache aqlFunctionsInternalCache;
    transaction::Methods& trx;
    int64_t stopOptimizationRequests = 0;
    int64_t nestingLevel = 0;
    int64_t filterDepth = -1;  // -1 = not in filter
    bool hasSeenAnyWriteNode = false;
    bool hasSeenWriteNodeInCurrentScope = false;
  };

  TraversalContext context(trx);

  auto preVisitor = [&](AstNode const* node) -> bool {
    auto ctx = &context;
    if (ctx->filterDepth >= 0) {
      ++ctx->filterDepth;
    }

    if (node->type == NODE_TYPE_FILTER) {
      TRI_ASSERT(ctx->filterDepth == -1);
      ctx->filterDepth = 0;
    } else if (node->type == NODE_TYPE_TRAVERSAL) {
      size_t parallelism = extractParallelism(node->getMember(4));
      if (parallelism > 1) {
        setContainsParallelNode();
      }
    } else if (node->type == NODE_TYPE_FCALL) {
      auto func = static_cast<Function*>(node->getData());
      TRI_ASSERT(func != nullptr);

      if (func->hasFlag(Function::Flags::NoEval)) {
        // NOOPT will turn all function optimizations off
        ++(ctx->stopOptimizationRequests);
      }
      if (node->willUseV8()) {
        setWillUseV8();
      }
    } else if (node->type == NODE_TYPE_FCALL_USER) {
      // user-defined function. will always use V8
      setWillUseV8();
    } else if (node->type == NODE_TYPE_COLLECTION_LIST) {
      // a collection list is produced by WITH a, b, c
      // or by traversal declarations
      // we do not want to descend further, in order to not track
      // the collections in collectionsFirstSeen
      return false;
    } else if (node->type == NODE_TYPE_COLLECTION) {
      // note the level on which we first saw a collection
      ctx->collectionsFirstSeen.try_emplace(node->getString(), ctx->nestingLevel);
    } else if (node->type == NODE_TYPE_AGGREGATIONS) {
      ++ctx->stopOptimizationRequests;
    } else if (node->type == NODE_TYPE_SUBQUERY) {
      ++ctx->nestingLevel;
    } else if (node->hasFlag(FLAG_BIND_PARAMETER)) {
      return false;
    } else if (node->type == NODE_TYPE_REMOVE || node->type == NODE_TYPE_INSERT ||
               node->type == NODE_TYPE_UPDATE || node->type == NODE_TYPE_REPLACE ||
               node->type == NODE_TYPE_UPSERT) {
      if (ctx->hasSeenWriteNodeInCurrentScope) {
        // no two data-modification nodes are allowed in the same scope
        THROW_ARANGO_EXCEPTION(TRI_ERROR_QUERY_MULTI_MODIFY);
      }
      ctx->hasSeenWriteNodeInCurrentScope = true;
    } else if (node->type == NODE_TYPE_VALUE) {
      return false;
    } else if (node->type == NODE_TYPE_ARRAY && node->isConstant()) {
      // nothing to be done in constant arrays
      return false;
    }

    return true;
  };

  auto postVisitor = [&](AstNode const* node) -> void {
    auto ctx = &context;
    if (ctx->filterDepth >= 0) {
      --ctx->filterDepth;
    }

    if (node->type == NODE_TYPE_FILTER) {
      ctx->filterDepth = -1;
    } else if (node->type == NODE_TYPE_SUBQUERY) {
      --ctx->nestingLevel;
    } else if (node->type == NODE_TYPE_REMOVE || node->type == NODE_TYPE_INSERT ||
               node->type == NODE_TYPE_UPDATE || node->type == NODE_TYPE_REPLACE ||
               node->type == NODE_TYPE_UPSERT) {
      ctx->hasSeenAnyWriteNode = true;

      TRI_ASSERT(ctx->hasSeenWriteNodeInCurrentScope);
      ctx->hasSeenWriteNodeInCurrentScope = false;

      auto collection = node->getMember(1);
      std::string name = collection->getString();
      ctx->writeCollectionsSeen.emplace(name);

      auto it = ctx->collectionsFirstSeen.find(name);

      if (it != ctx->collectionsFirstSeen.end()) {
        if ((*it).second < ctx->nestingLevel) {
          name = "collection '" + name;
          name.push_back('\'');
          THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_ACCESS_AFTER_MODIFICATION,
                                        name.c_str());
        }
      }
    } else if (node->type == NODE_TYPE_FCALL) {
      auto func = static_cast<Function*>(node->getData());
      TRI_ASSERT(func != nullptr);

      if (func->hasFlag(Function::Flags::NoEval)) {
        // NOOPT will turn all function optimizations off
        --ctx->stopOptimizationRequests;
      }
    } else if (node->type == NODE_TYPE_AGGREGATIONS) {
      --ctx->stopOptimizationRequests;
    } else if (node->type == NODE_TYPE_ARRAY && node->hasFlag(DETERMINED_CONSTANT) &&
               !node->hasFlag(VALUE_CONSTANT) && node->numMembers() < 10) {
      // optimization attempt: we are speculating that this array contains function
      // call parameters, which may have been optimized somehow.
      // if the array is marked as non-const, we remove this non-const marker so its
      // constness will be checked upon next attempt again
      // this allows optimizing cases such as FUNC1(FUNC2(...)):
      // in this case, due to the depth-first traversal we will first optimize FUNC2(...)
      // and replace it with a constant value. This will turn the Ast into FUNC1(const),
      // which may be optimized further if FUNC1 is deterministic.
      // However, function parameters are stored in ARRAY Ast nodes, which do not have
      // a back pointer to the actual function, so all we can do here is guess and
      // speculate that the array contained actual function call parameters
      // note: the max array length of 10 is chosen arbitrarily based on the assumption
      // that function calls normally have few parameters only, and it puts a cap
      // on the additional costs of having to re-calculate the const-determination flags
      // for all array members later on
      node->removeFlag(DETERMINED_CONSTANT);
      node->removeFlag(VALUE_CONSTANT);
    }
  };

  auto visitor = [&](AstNode* node) -> AstNode* {
    if (node == nullptr) {
      return nullptr;
    }

    auto ctx = &context;

    // unary operators
    if (node->type == NODE_TYPE_OPERATOR_UNARY_PLUS ||
        node->type == NODE_TYPE_OPERATOR_UNARY_MINUS) {
      return this->optimizeUnaryOperatorArithmetic(node);
    }

    if (node->type == NODE_TYPE_OPERATOR_UNARY_NOT) {
      return this->optimizeUnaryOperatorLogical(node);
    }

    // binary operators
    if (node->type == NODE_TYPE_OPERATOR_BINARY_AND || node->type == NODE_TYPE_OPERATOR_BINARY_OR) {
      return this->optimizeBinaryOperatorLogical(node, ctx->filterDepth == 1);
    }

    if (node->type == NODE_TYPE_OPERATOR_BINARY_EQ ||
        node->type == NODE_TYPE_OPERATOR_BINARY_NE || node->type == NODE_TYPE_OPERATOR_BINARY_LT ||
        node->type == NODE_TYPE_OPERATOR_BINARY_LE || node->type == NODE_TYPE_OPERATOR_BINARY_GT ||
        node->type == NODE_TYPE_OPERATOR_BINARY_GE || node->type == NODE_TYPE_OPERATOR_BINARY_IN ||
        node->type == NODE_TYPE_OPERATOR_BINARY_NIN) {
      return this->optimizeBinaryOperatorRelational(ctx->trx, ctx->aqlFunctionsInternalCache, node);
    }

    if (node->type == NODE_TYPE_OPERATOR_BINARY_PLUS ||
        node->type == NODE_TYPE_OPERATOR_BINARY_MINUS ||
        node->type == NODE_TYPE_OPERATOR_BINARY_TIMES || node->type == NODE_TYPE_OPERATOR_BINARY_DIV ||
        node->type == NODE_TYPE_OPERATOR_BINARY_MOD) {
      return this->optimizeBinaryOperatorArithmetic(node);
    }

    // ternary operator
    if (node->type == NODE_TYPE_OPERATOR_TERNARY) {
      return this->optimizeTernaryOperator(node);
    }

    // attribute access
    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      return this->optimizeAttributeAccess(node, ctx->variableDefinitions);
    }

    // passthru node
    if (node->type == NODE_TYPE_PASSTHRU) {
      // optimize away passthru node. this type of node is only used during
      // parsing
      return node->getMember(0);
    }

    // call to built-in function
    if (node->type == NODE_TYPE_FCALL) {
      auto func = static_cast<Function*>(node->getData());

      if (ctx->hasSeenAnyWriteNode && !func->hasFlag(Function::Flags::CanRunOnDBServer)) {
        // if canRunOnDBServer is true, then this is an indicator for a
        // document-accessing function
        std::string name("function ");
        name.append(func->name);
        THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_ACCESS_AFTER_MODIFICATION,
                                      name.c_str());
      }

      if (ctx->stopOptimizationRequests == 0) {
        // optimization allowed
        return this->optimizeFunctionCall(ctx->trx, ctx->aqlFunctionsInternalCache, node);
      }
      // optimization not allowed
      return node;
    }

    // reference to a variable, may be able to insert the variable value
    // directly
    if (node->type == NODE_TYPE_REFERENCE) {
      return this->optimizeReference(node);
    }

    // indexed access, e.g. a[0] or a['foo']
    if (node->type == NODE_TYPE_INDEXED_ACCESS) {
      return this->optimizeIndexedAccess(node);
    }

    // LET
    if (node->type == NODE_TYPE_LET) {
      // remember variable assignments
      TRI_ASSERT(node->numMembers() == 2);
      Variable const* variable =
          static_cast<Variable const*>(node->getMember(0)->getData());
      AstNode const* definition = node->getMember(1);
      // recursively process assignments so we can track LET a = b LET c = b

      while (definition->type == NODE_TYPE_REFERENCE) {
        auto it = ctx->variableDefinitions.find(
            static_cast<Variable const*>(definition->getData()));
        if (it == ctx->variableDefinitions.end()) {
          break;
        }
        definition = (*it).second;
      }

      ctx->variableDefinitions.try_emplace(variable, definition);
      return this->optimizeLet(node);
    }

    // FILTER
    if (node->type == NODE_TYPE_FILTER) {
      return this->optimizeFilter(node);
    }

    // FOR
    if (node->type == NODE_TYPE_FOR) {
      return this->optimizeFor(node);
    }

    // collection
    if (node->type == NODE_TYPE_COLLECTION) {
      if (ctx->writeCollectionsSeen.find(node->getString()) !=
          ctx->writeCollectionsSeen.end()) {
        std::string name("collection '");
        name.append(node->getString());
        name.push_back('\'');
        THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_ACCESS_AFTER_MODIFICATION,
                                      name.c_str());
      }

      return node;
    }

    if (node->type == NODE_TYPE_OBJECT) {
      return this->optimizeObject(node);
    }

    // traversal
    if (node->type == NODE_TYPE_TRAVERSAL) {
      // traversals must not be used after a modification operation
      if (ctx->hasSeenAnyWriteNode) {
        THROW_ARANGO_EXCEPTION_PARAMS(TRI_ERROR_QUERY_ACCESS_AFTER_MODIFICATION,
                                      "traversal");
      }

      return node;
    }

    // example
    if (node->type == NODE_TYPE_EXAMPLE) {
      return this->makeConditionFromExample(node);
    }

    return node;
  };

  // run the optimizations
  _root = traverseAndModify(_root, preVisitor, visitor, postVisitor);
}

/// @brief determines the variables referenced in an expression
void Ast::getReferencedVariables(AstNode const* node, VarSet& result) {
  auto preVisitor = [](AstNode const* node) -> bool {
    return !node->isConstant();
  };

  auto visitor = [&result](AstNode const* node) {
    if (node == nullptr) {
      return;
    }

    // reference to a variable
    if (node->type == NODE_TYPE_REFERENCE) {
      auto variable = static_cast<Variable const*>(node->getData());

      if (variable == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "invalid reference in AST");
      }

      if (variable->needsRegister()) {
        result.emplace(variable);
      }
    }
  };

  traverseReadOnly(node, preVisitor, visitor);
}

/// @brief count how many times a variable is referenced in an expression
size_t Ast::countReferences(AstNode const* node, Variable const* search) {
  struct CountResult {
    size_t count;
    Variable const* search;
  };

  CountResult result{0, search};
  auto visitor = [&result](AstNode const* node) {
    if (node == nullptr) {
      return;
    }

    // reference to a variable
    if (node->type == NODE_TYPE_REFERENCE) {
      auto variable = static_cast<Variable const*>(node->getData());

      if (variable->id == result.search->id) {
        ++result.count;
      }
    }
  };

  traverseReadOnly(node, visitor);
  return result.count;
}

/// @brief determines the top-level attributes referenced in an expression,
/// grouped by variable name
TopLevelAttributes Ast::getReferencedAttributes(AstNode const* node, bool& isSafeForOptimization) {
  TopLevelAttributes result;

  // traversal state
  char const* attributeName = nullptr;
  size_t nameLength = 0;
  isSafeForOptimization = true;

  auto visitor = [&](AstNode const* node) {
    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      attributeName = node->getStringValue();
      nameLength = node->getStringLength();
      return true;
    }

    if (node->type == NODE_TYPE_REFERENCE) {
      // reference to a variable
      if (attributeName == nullptr) {
        // we haven't seen an attribute access directly before...
        // this may have been an access to an indexed property, e.g value[0] or
        // a reference to the complete value, e.g. FUNC(value)
        // note that this is unsafe to optimize this away
        isSafeForOptimization = false;
        return true;
      }

      TRI_ASSERT(attributeName != nullptr);

      auto variable = static_cast<Variable const*>(node->getData());

      if (variable == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
      }

      auto[it, emp] = result.try_emplace(
        variable,
        arangodb::lazyConstruct([&]{
         return std::unordered_set<std::string>( {std::string(attributeName, nameLength)});
         })
      );
      if (emp) {
        // insert attributeName only
        (*it).second.emplace(attributeName, nameLength);
      }

      // fall-through
    }

    attributeName = nullptr;
    nameLength = 0;
    return true;
  };

  traverseReadOnly(node, visitor, ::doNothingVisitor);

  return result;
}

/// @brief determines the to-be-kept attributes of an INTO expression
std::unordered_set<std::string> Ast::getReferencedAttributesForKeep(
    AstNode const* node, Variable const* searchVariable, bool& isSafeForOptimization) {
  auto isTargetVariable = [&searchVariable](AstNode const* node) {
    if (node->type == NODE_TYPE_INDEXED_ACCESS) {
      auto sub = node->getMemberUnchecked(0);
      if (sub->type == NODE_TYPE_REFERENCE) {
        Variable const* v = static_cast<Variable const*>(sub->getData());
        if (v->id == searchVariable->id) {
          return true;
        }
      }
    } else if (node->type == NODE_TYPE_EXPANSION) {
      if (node->numMembers() < 2) {
        return false;
      }
      auto it = node->getMemberUnchecked(0);
      if (it->type != NODE_TYPE_ITERATOR || it->numMembers() != 2) {
        return false;
      }

      auto sub1 = it->getMember(0);
      auto sub2 = it->getMember(1);
      if (sub1->type != NODE_TYPE_VARIABLE || sub2->type != NODE_TYPE_REFERENCE) {
        return false;
      }
      Variable const* v = static_cast<Variable const*>(sub2->getData());
      if (v->id == searchVariable->id) {
        return true;
      }
    }

    return false;
  };

  std::unordered_set<std::string> result;
  isSafeForOptimization = true;

  auto visitor = [&isSafeForOptimization,
                  &result, &isTargetVariable,
                  &searchVariable](AstNode const* node) {
    if (!isSafeForOptimization) {
      return false;
    }

    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      while (node->getMemberUnchecked(0)->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
        node = node->getMemberUnchecked(0);
      }
      if (isTargetVariable(node->getMemberUnchecked(0))) {
        result.emplace(node->getString());
        // do not descend further
        return false;
      }
    } else if (node->type == NODE_TYPE_REFERENCE) {
      Variable const* v = static_cast<Variable const*>(node->getData());
      if (v->id == searchVariable->id) {
        isSafeForOptimization = false;
        return false;
      }
    } else if (node->type == NODE_TYPE_EXPANSION) {
      if (isTargetVariable(node)) {
        auto sub = node->getMemberUnchecked(1);
        if (sub->type == NODE_TYPE_EXPANSION) {
          sub = sub->getMemberUnchecked(0)->getMemberUnchecked(1);
        }
        if (sub->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
          while (sub->getMemberUnchecked(0)->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
            sub = sub->getMemberUnchecked(0);
          }
          result.emplace(sub->getString());
          // do not descend further
          return false;
        }
      }
    }

    return true;
  };

  traverseReadOnly(node, visitor, ::doNothingVisitor);

  return result;
}

/// @brief determines the top-level attributes referenced in an expression for
/// the specified out variable
bool Ast::getReferencedAttributes(AstNode const* node, Variable const* variable,
                                  std::unordered_set<std::string>& vars) {
  // traversal state
  struct TraversalState {
    Variable const* variable;
    char const* attributeName;
    size_t nameLength;
    bool isSafeForOptimization;
    std::unordered_set<std::string>& vars;
  };

  TraversalState state{variable, nullptr, 0, true, vars};

  auto visitor = [&state](AstNode const* node) -> bool {
    if (node == nullptr || !state.isSafeForOptimization) {
      return false;
    }

    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      state.attributeName = node->getStringValue();
      state.nameLength = node->getStringLength();
      return true;
    }

    if (node->type == NODE_TYPE_REFERENCE) {
      // reference to a variable
      auto v = static_cast<Variable const*>(node->getData());
      if (v == state.variable) {
        if (state.attributeName == nullptr) {
          // we haven't seen an attribute access directly before...
          // this may have been an access to an indexed property, e.g value[0]
          // or a reference to the complete value, e.g. FUNC(value) note that
          // this is unsafe to optimize this away
          state.isSafeForOptimization = false;
          return false;
        }
        // insert attributeName only
        state.vars.emplace(state.attributeName, state.nameLength);
      }

      // fall-through
    }

    state.attributeName = nullptr;
    state.nameLength = 0;

    return true;
  };

  traverseReadOnly(node, visitor, ::doNothingVisitor);

  return state.isSafeForOptimization;
}

/// @brief determines the attributes (and their subattributes) referenced in an 
/// expression for the specified out variable
bool Ast::getReferencedAttributesRecursive(AstNode const* node, Variable const* variable,
                                           std::unordered_set<arangodb::aql::AttributeNamePath>& vars) {
  // traversal state
  struct TraversalState {
    Variable const* variable;
    bool isSafeForOptimization;
    std::unordered_set<arangodb::aql::AttributeNamePath>& vars;
    arangodb::aql::AttributeNamePath path;
  };

  TraversalState state{variable, true, vars, {}};

  auto visitor = [&state](AstNode const* node) -> bool {
    if (node == nullptr || !state.isSafeForOptimization) {
      return false;
    }

    if (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
      state.path.path.emplace_back(node->getStringValue(), node->getStringLength());
      return true;
    }

    if (node->type == NODE_TYPE_REFERENCE) {
      // reference to a variable
      auto v = static_cast<Variable const*>(node->getData());
      if (v == state.variable) {
        if (state.path.empty()) {
          // we haven't seen an attribute access directly before...
          // this may have been an access to an indexed property, e.g value[0]
          // or a reference to the complete value, e.g. FUNC(value) note that
          // this is unsafe to optimize this away
          state.isSafeForOptimization = false;
          return false;
        }
        // we picked attribute names up in reverse order, so now reverse them
        state.path.reverse();
        state.vars.emplace(std::move(state.path.path));
        state.path.clear();
      }
      // fall-through
    }

    state.path.clear();
    return true;
  };

  traverseReadOnly(node, visitor, ::doNothingVisitor);

  return state.isSafeForOptimization;
}

/// @brief copies node payload from node into copy. this is *not* copying
/// the subnodes
void Ast::copyPayload(AstNode const* node, AstNode* copy) const {
  TRI_ASSERT(!copy->hasFlag(AstNodeFlagType::FLAG_INTERNAL_CONST));
  TRI_ASSERT(copy->computedValue() == nullptr);

  AstNodeType const type = node->type;

  if (type == NODE_TYPE_COLLECTION || type == NODE_TYPE_VIEW || type == NODE_TYPE_PARAMETER ||
      type == NODE_TYPE_PARAMETER_DATASOURCE || type == NODE_TYPE_ATTRIBUTE_ACCESS ||
      type == NODE_TYPE_OBJECT_ELEMENT || type == NODE_TYPE_FCALL_USER) {
    copy->setStringValue(node->getStringValue(), node->getStringLength());
  } else if (type == NODE_TYPE_VARIABLE || type == NODE_TYPE_REFERENCE || type == NODE_TYPE_FCALL) {
    copy->setData(node->getData());
  } else if (type == NODE_TYPE_UPSERT || type == NODE_TYPE_EXPANSION) {
    copy->setIntValue(node->getIntValue(true));
  } else if (type == NODE_TYPE_QUANTIFIER) {
    copy->setIntValue(node->getIntValue(true));
  } else if (type == NODE_TYPE_OPERATOR_BINARY_LE || type == NODE_TYPE_OPERATOR_BINARY_LT ||
             type == NODE_TYPE_OPERATOR_BINARY_EQ) {
    // copy "definitely is not null" information
    copy->setExcludesNull(node->getExcludesNull());
  } else if (type == NODE_TYPE_OPERATOR_BINARY_IN || type == NODE_TYPE_OPERATOR_BINARY_NIN ||
             type == NODE_TYPE_OPERATOR_BINARY_ARRAY_IN ||
             type == NODE_TYPE_OPERATOR_BINARY_ARRAY_NIN) {
    // copy sortedness information
    copy->setBoolValue(node->getBoolValue());
  } else if (type == NODE_TYPE_ARRAY) {
    if (node->isSorted()) {
      copy->setFlag(DETERMINED_SORTED, VALUE_SORTED);
    } else {
      copy->setFlag(DETERMINED_SORTED);
    }
  } else if (type == NODE_TYPE_VALUE) {
    switch (node->value.type) {
      case VALUE_TYPE_NULL:
        copy->value.type = VALUE_TYPE_NULL;
        break;
      case VALUE_TYPE_BOOL:
        copy->value.type = VALUE_TYPE_BOOL;
        copy->setBoolValue(node->getBoolValue());
        break;
      case VALUE_TYPE_INT:
        copy->value.type = VALUE_TYPE_INT;
        copy->setIntValue(node->getIntValue());
        break;
      case VALUE_TYPE_DOUBLE:
        copy->value.type = VALUE_TYPE_DOUBLE;
        copy->setDoubleValue(node->getDoubleValue());
        break;
      case VALUE_TYPE_STRING:
        copy->value.type = VALUE_TYPE_STRING;
        copy->setStringValue(node->getStringValue(), node->getStringLength());
        break;
    }
  }
}

/// @brief recursively clone a node
AstNode* Ast::clone(AstNode const* node) {
  AstNodeType const type = node->type;
  if (type == NODE_TYPE_NOP) {
    // nop node is a singleton
    return const_cast<AstNode*>(node);
  }
  
  AstNode* copy = createNode(type);
  TRI_ASSERT(copy != nullptr);

  // copy flags, but nothing const-related
  copy->flags = node->flags;
  copy->removeFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);
  TEMPORARILY_UNLOCK_NODE(copy);  // if locked, unlock to copy properly
  
  // special handling for certain node types
  // copy payload...
  copyPayload(node, copy);
  
  // recursively clone subnodes
  size_t const n = node->numMembers();
  copy->members.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    copy->addMember(clone(node->getMemberUnchecked(i)));
  }

  return copy;
}

AstNode* Ast::shallowCopyForModify(AstNode const* node) {
  AstNodeType const type = node->type;
  if (type == NODE_TYPE_NOP) {
    // nop node is a singleton
    return const_cast<AstNode*>(node);
  }

  AstNode* copy = createNode(type);
  TRI_ASSERT(copy != nullptr);

  // copy flags
  copy->flags = node->flags;
  copy->removeFlag(AstNodeFlagType::FLAG_FINALIZED);
  copy->removeFlag(AstNodeFlagType::FLAG_INTERNAL_CONST);

  // special handling for certain node types
  // copy payload...
  copyPayload(node, copy);
  
  // recursively add subnodes
  size_t const n = node->numMembers();
  copy->members.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    copy->addMember(node->getMemberUnchecked(i));
  }

  return copy;
}

/// @brief deduplicate an array
/// will return the original node if no modifications were made, and a new
/// node if the array contained modifications
AstNode const* Ast::deduplicateArray(AstNode const* node) {
  TRI_ASSERT(node != nullptr);

  if (!node->isArray() || !node->isConstant()) {
    return node;
  }

  // ok, now we're sure node is a constant array
  size_t const n = node->numMembers();

  if (n <= 1) {
    // nothing to do
    return node;
  }
  
  VPackBuilder temp;

  if (node->isSorted()) {
    bool unique = true;
    auto member = node->getMemberUnchecked(0);
    VPackSlice lhs = member->computeValue(&temp);
    for (size_t i = 1; i < n; ++i) {
      VPackSlice rhs = node->getMemberUnchecked(i)->computeValue(&temp);

      if (arangodb::basics::VelocyPackHelper::equal(lhs, rhs, false, nullptr)) {
        unique = false;
        break;
      }

      lhs = rhs;
    }

    if (unique) {
      // array members are unique
      return node;
    }
  }

  // TODO: sort values in place first and compare two adjacent members each
  std::unordered_map<VPackSlice, AstNode const*, arangodb::basics::VelocyPackHelper::VPackHash,
                     arangodb::basics::VelocyPackHelper::VPackEqual>
      cache(n, arangodb::basics::VelocyPackHelper::VPackHash(),
            arangodb::basics::VelocyPackHelper::VPackEqual());

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);
    VPackSlice slice = member->computeValue(&temp);

    if (cache.find(slice) == cache.end()) {
      cache.try_emplace(slice, member);
    }
  }

  // we may have got duplicates. now create a copy of the deduplicated values
  auto copy = createNodeArray(cache.size());

  for (auto& it : cache) {
    copy->addMember(it.second);
  }
  copy->sort();

  return copy;
}

/// @brief check if an operator is reversible
bool Ast::IsReversibleOperator(AstNodeType type) {
  return (ReversedOperators.find(static_cast<int>(type)) != ReversedOperators.end());
}

/// @brief get the reversed operator for a comparison operator
AstNodeType Ast::ReverseOperator(AstNodeType type) {
  auto it = ReversedOperators.find(static_cast<int>(type));

  if (it == ReversedOperators.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid node type for inversed operator");
  }

  return (*it).second;
}

/// @brief get the n-ary operator type equivalent for a binary operator type
AstNodeType Ast::NaryOperatorType(AstNodeType old) {
  TRI_ASSERT(old == NODE_TYPE_OPERATOR_BINARY_AND || old == NODE_TYPE_OPERATOR_BINARY_OR);

  if (old == NODE_TYPE_OPERATOR_BINARY_AND) {
    return NODE_TYPE_OPERATOR_NARY_AND;
  }
  if (old == NODE_TYPE_OPERATOR_BINARY_OR) {
    return NODE_TYPE_OPERATOR_NARY_OR;
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "invalid node type for n-ary operator");
}

bool Ast::IsAndOperatorType(AstNodeType tt) {
  return tt == NODE_TYPE_OPERATOR_BINARY_AND || tt == NODE_TYPE_OPERATOR_NARY_AND;
}

bool Ast::IsOrOperatorType(AstNodeType tt) {
  return tt == NODE_TYPE_OPERATOR_BINARY_OR || tt == NODE_TYPE_OPERATOR_NARY_OR;
}

/// @brief make condition from example
AstNode* Ast::makeConditionFromExample(AstNode const* node) {
  TRI_ASSERT(node->numMembers() == 1);

  auto object = node->getMember(0);

  if (object->type != NODE_TYPE_OBJECT) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "expecting object literal for example");
  }

  auto variable = static_cast<AstNode*>(node->getData());

  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "internal error in object literal handling");
  }

  AstNode* result = nullptr;
  ::arangodb::containers::SmallVector<arangodb::velocypack::StringRef>::allocator_type::arena_type a;
  ::arangodb::containers::SmallVector<arangodb::velocypack::StringRef> attributeParts{a};

  std::function<void(AstNode const*)> createCondition = [&](AstNode const* object) -> void {
    TRI_ASSERT(object->type == NODE_TYPE_OBJECT);

    auto const n = object->numMembers();
    for (size_t i = 0; i < n; ++i) {
      auto member = object->getMember(i);

      if (member->type != NODE_TYPE_OBJECT_ELEMENT) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_QUERY_PARSE,
            "expecting object literal with literal attribute names in example");
      }

      attributeParts.emplace_back(member->getStringRef());

      auto value = member->getMember(0);

      if (value->type == NODE_TYPE_OBJECT && value->numMembers() != 0) {
        createCondition(value);
      } else {
        auto access = variable;
        for (auto const& it : attributeParts) {
          access = createNodeAttributeAccess(access, it.data(), it.size());
        }

        auto condition =
            createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_EQ, access, value);

        if (result == nullptr) {
          result = condition;
        } else {
          // AND-combine with previous condition
          result = createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_AND, result, condition);
        }
      }

      attributeParts.pop_back();
    }
  };

  createCondition(object);

  if (result == nullptr) {
    result = createNodeValueBool(true);
  }

  return result;
}

/// @brief create a number node for an arithmetic result, integer
AstNode* Ast::createArithmeticResultNode(int64_t value) {
  return createNodeValueInt(value);
}

/// @brief create a number node for an arithmetic result, double
AstNode* Ast::createArithmeticResultNode(double value) {
  if (value != value ||  // intentional!
      value == HUGE_VAL || value == -HUGE_VAL) {
    // IEEE754 NaN values have an interesting property that we can exploit...
    // if the architecture does not use IEEE754 values then this shouldn't do
    // any harm either
    _query.warnings().registerWarning(TRI_ERROR_QUERY_NUMBER_OUT_OF_RANGE);
    return const_cast<AstNode*>(&_specialNodes.NullNode);
  }

  return createNodeValueDouble(value);
}

/// @brief optimizes the unary operators + and -
/// the unary plus will be converted into a simple value node if the operand of
/// the operation is a constant number
AstNode* Ast::optimizeUnaryOperatorArithmetic(AstNode* node) {
  TRI_ASSERT(node != nullptr);

  TRI_ASSERT(node->type == NODE_TYPE_OPERATOR_UNARY_PLUS ||
             node->type == NODE_TYPE_OPERATOR_UNARY_MINUS);
  TRI_ASSERT(node->numMembers() == 1);

  AstNode* operand = node->getMember(0);
  if (!operand->isConstant()) {
    // operand is dynamic, cannot statically optimize it
    return node;
  }

  // operand is a constant, now convert it into a number
  AstNode const* converted = operand->castToNumber(this);

  if (converted->isNullValue()) {
    return const_cast<AstNode*>(&_specialNodes.ZeroNode);
  }

  if (converted->value.type != VALUE_TYPE_INT && converted->value.type != VALUE_TYPE_DOUBLE) {
    // non-numeric operand
    return node;
  }

  if (node->type == NODE_TYPE_OPERATOR_UNARY_PLUS) {
    // + number => number
    return const_cast<AstNode*>(converted);
  } else {
    // - number
    if (converted->value.type == VALUE_TYPE_INT) {
      // int64
      return createNodeValueInt(-converted->getIntValue());
    } else {
      // double
      double const value = -converted->getDoubleValue();

      if (value != value ||  // intentional
          value == HUGE_VAL || value == -HUGE_VAL) {
        // IEEE754 NaN values have an interesting property that we can
        // exploit...
        // if the architecture does not use IEEE754 values then this shouldn't
        // do
        // any harm either
        return const_cast<AstNode*>(&_specialNodes.ZeroNode);
      }

      return createNodeValueDouble(value);
    }
  }

  TRI_ASSERT(false);
}

/// @brief optimizes the unary operator NOT
/// the unary NOT operation will be replaced with the result of the operation
AstNode* Ast::optimizeNotExpression(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  if (node->type != NODE_TYPE_OPERATOR_UNARY_NOT) {
    return node;
  }

  TRI_ASSERT(node->type == NODE_TYPE_OPERATOR_UNARY_NOT);
  TRI_ASSERT(node->numMembers() == 1);

  AstNode* operand = node->getMember(0);

  if (operand->isComparisonOperator()) {
    // remove the NOT and reverse the operation, e.g. NOT (a == b) => (a != b)
    TRI_ASSERT(operand->numMembers() == 2);
    auto lhs = operand->getMember(0);
    auto rhs = operand->getMember(1);

    auto it = NegatedOperators.find(static_cast<int>(operand->type));
    TRI_ASSERT(it != NegatedOperators.end());

    return createNodeBinaryOperator((*it).second, lhs, rhs);
  }

  return node;
}

/// @brief optimizes the unary operator NOT
/// the unary NOT operation will be replaced with the result of the operation
AstNode* Ast::optimizeUnaryOperatorLogical(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_OPERATOR_UNARY_NOT);
  TRI_ASSERT(node->numMembers() == 1);

  AstNode* operand = node->getMember(0);
  if (!operand->isConstant()) {
    // operand is dynamic, cannot statically optimize it
    return optimizeNotExpression(node);
  }

  AstNode const* converted = operand->castToBool(this);

  // replace unary negation operation with result of negation
  return createNodeValueBool(!converted->getBoolValue());
}

/// @brief optimizes the binary logical operators && and ||
AstNode* Ast::optimizeBinaryOperatorLogical(AstNode* node, bool canModifyResultType) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_OPERATOR_BINARY_AND ||
             node->type == NODE_TYPE_OPERATOR_BINARY_OR);
  TRI_ASSERT(node->numMembers() == 2);

  auto lhs = node->getMember(0);
  auto rhs = node->getMember(1);

  if (lhs == nullptr || rhs == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (lhs->isConstant()) {
    // left operand is a constant value
    if (node->type == NODE_TYPE_OPERATOR_BINARY_AND) {
      if (lhs->isFalse()) {
        // return it if it is falsey
        return lhs;
      }

      // left-operand was trueish, now return right operand
      return rhs;
    } else if (node->type == NODE_TYPE_OPERATOR_BINARY_OR) {
      if (lhs->isTrue()) {
        // return it if it is trueish
        return lhs;
      }

      // left-operand was falsey, now return right operand
      return rhs;
    }
  }

  if (canModifyResultType) {
    if (rhs->isConstant() && lhs->isDeterministic()) {
      // right operand is a constant value
      if (node->type == NODE_TYPE_OPERATOR_BINARY_AND) {
        if (rhs->isFalse()) {
          return createNodeValueBool(false);
        }

        // right-operand was trueish, now return just left
        return lhs;
      } else if (node->type == NODE_TYPE_OPERATOR_BINARY_OR) {
        if (rhs->isTrue()) {
          // return it if it is trueish
          return createNodeValueBool(true);
        }

        // right-operand was falsey, now return left operand
        return lhs;
      }
    }
  }

  // default case
  return node;
}

/// @brief optimizes the binary relational operators <, <=, >, >=, ==, != and IN
AstNode* Ast::optimizeBinaryOperatorRelational(transaction::Methods& trx,
                                               AqlFunctionsInternalCache& aqlFunctionsInternalCache,
                                               AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->numMembers() == 2);

  AstNode* lhs = node->getMember(0);
  AstNode* rhs = node->getMember(1);

  if (lhs == nullptr || rhs == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (lhs->isDeterministic() && rhs->type == NODE_TYPE_ARRAY && rhs->numMembers() <= 1 &&
      (node->type == NODE_TYPE_OPERATOR_BINARY_IN || node->type == NODE_TYPE_OPERATOR_BINARY_NIN)) {
    // turn an IN or a NOT IN with few members into an equality comparison
    if (rhs->numMembers() == 0) {
      // IN with no members returns false
      // NOT IN with no members returns true
      return createNodeValueBool(node->type == NODE_TYPE_OPERATOR_BINARY_NIN);
    } else if (rhs->numMembers() == 1) {
      // IN with a single member becomes equality
      // NOT IN with a single members becomes unequality
      if (node->type == NODE_TYPE_OPERATOR_BINARY_IN) {
        node = createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_EQ, lhs,
                                        rhs->getMember(0));
      } else {
        node = createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_NE, lhs,
                                        rhs->getMember(0));
      }
      // and optimize ourselves...
      return optimizeBinaryOperatorRelational(trx, aqlFunctionsInternalCache, node);
    }
    // intentionally falls through
  }

  bool const rhsIsConst = rhs->isConstant();

  if (!rhsIsConst) {
    return node;
  }

  if (rhs->type != NODE_TYPE_ARRAY && (node->type == NODE_TYPE_OPERATOR_BINARY_IN ||
                                       node->type == NODE_TYPE_OPERATOR_BINARY_NIN)) {
    // right operand of IN or NOT IN must be an array or a range, otherwise we
    // return false
    return createNodeValueBool(false);
  }

  bool const lhsIsConst = lhs->isConstant();

  if (!lhsIsConst) {
    if (rhs->numMembers() >= AstNode::SortNumberThreshold && rhs->type == NODE_TYPE_ARRAY &&
        (node->type == NODE_TYPE_OPERATOR_BINARY_IN || node->type == NODE_TYPE_OPERATOR_BINARY_NIN)) {
      // if the IN list contains a considerable amount of items, we will sort
      // it, so we can find elements quicker later using a binary search
      // note that sorting will also set a flag for the node

      // first copy the original node before sorting, as the node may be used
      // somewhere else too
      rhs = clone(rhs);
      node->changeMember(1, rhs);
      rhs->sort();
      // remove the sortedness bit for IN/NIN operator node, as the operand is
      // now sorted
      node->setBoolValue(false);
    }

    return node;
  }

  TRI_ASSERT(lhs->isConstant() && rhs->isConstant());

  Expression exp(this, node);
  FixedVarExpressionContext context(trx, _query, aqlFunctionsInternalCache);
  bool mustDestroy;

  AqlValue a = exp.execute(&context, mustDestroy);
  AqlValueGuard guard(a, mustDestroy);

  AqlValueMaterializer materializer(trx.transactionContextPtr()->getVPackOptions());
  return nodeFromVPack(materializer.slice(a, false), true);
}

/// @brief optimizes the binary arithmetic operators +, -, *, / and %
AstNode* Ast::optimizeBinaryOperatorArithmetic(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->numMembers() == 2);

  AstNode* lhs = node->getMember(0);
  AstNode* rhs = node->getMember(1);

  if (lhs == nullptr || rhs == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (lhs->isConstant() && rhs->isConstant()) {
    // now calculate the expression result
    if (node->type == NODE_TYPE_OPERATOR_BINARY_PLUS) {
      // arithmetic +
      AstNode const* left = lhs->castToNumber(this);
      AstNode const* right = rhs->castToNumber(this);

      bool useDoublePrecision = (left->isDoubleValue() || right->isDoubleValue());

      if (!useDoublePrecision) {
        auto l = left->getIntValue();
        auto r = right->getIntValue();
        // check if the result would overflow
        useDoublePrecision = isUnsafeAddition<int64_t>(l, r);

        if (!useDoublePrecision) {
          // can calculate using integers
          return createArithmeticResultNode(l + r);
        }
      }

      // must use double precision
      return createArithmeticResultNode(left->getDoubleValue() + right->getDoubleValue());
    } else if (node->type == NODE_TYPE_OPERATOR_BINARY_MINUS) {
      AstNode const* left = lhs->castToNumber(this);
      AstNode const* right = rhs->castToNumber(this);

      bool useDoublePrecision = (left->isDoubleValue() || right->isDoubleValue());

      if (!useDoublePrecision) {
        auto l = left->getIntValue();
        auto r = right->getIntValue();
        // check if the result would overflow
        useDoublePrecision = isUnsafeSubtraction<int64_t>(l, r);

        if (!useDoublePrecision) {
          // can calculate using integers
          return createArithmeticResultNode(l - r);
        }
      }

      // must use double precision
      return createArithmeticResultNode(left->getDoubleValue() - right->getDoubleValue());
    } else if (node->type == NODE_TYPE_OPERATOR_BINARY_TIMES) {
      AstNode const* left = lhs->castToNumber(this);
      AstNode const* right = rhs->castToNumber(this);

      bool useDoublePrecision = (left->isDoubleValue() || right->isDoubleValue());

      if (!useDoublePrecision) {
        auto l = left->getIntValue();
        auto r = right->getIntValue();
        // check if the result would overflow
        useDoublePrecision = isUnsafeMultiplication<int64_t>(l, r);

        if (!useDoublePrecision) {
          // can calculate using integers
          return createArithmeticResultNode(l * r);
        }
      }

      // must use double precision
      return createArithmeticResultNode(left->getDoubleValue() * right->getDoubleValue());
    } else if (node->type == NODE_TYPE_OPERATOR_BINARY_DIV) {
      AstNode const* left = lhs->castToNumber(this);
      AstNode const* right = rhs->castToNumber(this);

      bool useDoublePrecision = (left->isDoubleValue() || right->isDoubleValue());
      if (!useDoublePrecision) {
        auto l = left->getIntValue();
        auto r = right->getIntValue();

        if (r == 0) {
          _query.warnings().registerWarning(TRI_ERROR_QUERY_DIVISION_BY_ZERO);
          return const_cast<AstNode*>(&_specialNodes.NullNode);
        }

        // check if the result would overflow
        useDoublePrecision = (isUnsafeDivision<int64_t>(l, r) || r < -1 || r > 1);

        if (!useDoublePrecision) {
          // can calculate using integers
          return createArithmeticResultNode(l / r);
        }
      }

      if (right->getDoubleValue() == 0.0) {
        _query.warnings().registerWarning(TRI_ERROR_QUERY_DIVISION_BY_ZERO);
        return const_cast<AstNode*>(&_specialNodes.NullNode);
      }

      return createArithmeticResultNode(left->getDoubleValue() / right->getDoubleValue());
    } else if (node->type == NODE_TYPE_OPERATOR_BINARY_MOD) {
      AstNode const* left = lhs->castToNumber(this);
      AstNode const* right = rhs->castToNumber(this);

      bool useDoublePrecision = (left->isDoubleValue() || right->isDoubleValue());
      if (!useDoublePrecision) {
        auto l = left->getIntValue();
        auto r = right->getIntValue();

        if (r == 0) {
          _query.warnings().registerWarning(TRI_ERROR_QUERY_DIVISION_BY_ZERO);
          return const_cast<AstNode*>(&_specialNodes.NullNode);
        }

        // check if the result would overflow
        useDoublePrecision = isUnsafeDivision<int64_t>(l, r);

        if (!useDoublePrecision) {
          // can calculate using integers
          return createArithmeticResultNode(l % r);
        }
      }

      if (right->getDoubleValue() == 0.0) {
        _query.warnings().registerWarning(TRI_ERROR_QUERY_DIVISION_BY_ZERO);
        return const_cast<AstNode*>(&_specialNodes.NullNode);
      }

      return createArithmeticResultNode(
          fmod(left->getDoubleValue(), right->getDoubleValue()));
    }

    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "invalid operator");
  }

  return node;
}

/// @brief optimizes the ternary operator
/// if the condition is constant, the operator will be replaced with either the
/// true part or the false part
AstNode* Ast::optimizeTernaryOperator(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_OPERATOR_TERNARY);
  TRI_ASSERT(node->numMembers() >= 2 && node->numMembers() <= 3);

  AstNode* condition = node->getMember(0);
  AstNode* truePart = (node->numMembers() == 2) ? condition : node->getMember(1);
  AstNode* falsePart = (node->numMembers() == 2) ? node->getMember(1) : node->getMember(2);

  if (condition == nullptr || truePart == nullptr || falsePart == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  if (!condition->isConstant()) {
    return node;
  }

  if (condition->isTrue()) {
    // condition is always true, replace ternary operation with true part
    return truePart;
  }

  // condition is always false, replace ternary operation with false part
  return falsePart;
}

/// @brief optimizes an attribute access
AstNode* Ast::optimizeAttributeAccess(
    AstNode* node, std::unordered_map<Variable const*, AstNode const*> const& variableDefinitions) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_ATTRIBUTE_ACCESS);
  TRI_ASSERT(node->numMembers() == 1);

  AstNode const* what = node->getMember(0);

  if (what->type == NODE_TYPE_REFERENCE) {
    // check if the access value is a variable and if it is an alias
    auto it = variableDefinitions.find(static_cast<Variable const*>(what->getData()));

    if (it != variableDefinitions.end()) {
      what = (*it).second;
    }
  }

  if (!what->isConstant()) {
    return node;
  }

  if (what->type == NODE_TYPE_OBJECT) {
    // accessing an attribute from an object
    char const* name = node->getStringValue();
    size_t const length = node->getStringLength();

    size_t const n = what->numMembers();

    for (size_t i = 0; i < n; ++i) {
      AstNode const* member = what->getMember(i);

      if (member->type == NODE_TYPE_OBJECT_ELEMENT && member->getStringLength() == length &&
          memcmp(name, member->getStringValue(), length) == 0) {
        // found matching member
        return member->getMember(0);
      }
    }
  }

  return node;
}

/// @brief optimizes a call to a built-in function
AstNode* Ast::optimizeFunctionCall(transaction::Methods& trx,
                                   AqlFunctionsInternalCache& aqlFunctionsInternalCache, AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_FCALL);
  TRI_ASSERT(node->numMembers() == 1);

  auto func = static_cast<Function*>(node->getData());
  TRI_ASSERT(func != nullptr);

  // simple replacements for some specific functions
  if (func->name == "LENGTH" || func->name == "COUNT") {
    // shortcut LENGTH(collection) to COLLECTION_COUNT(collection)
    auto args = node->getMember(0);
    if (args->numMembers() == 1) {
      auto arg = args->getMember(0);
      if (arg->type == NODE_TYPE_COLLECTION) {
        auto countArgs = createNodeArray();
        countArgs->addMember(createNodeValueString(arg->getStringValue(),
                                                   arg->getStringLength()));
        return createNodeFunctionCall(TRI_CHAR_LENGTH_PAIR("COLLECTION_COUNT"), countArgs);
      }
    }
  } else if (func->name == "IS_NULL") {
    auto args = node->getMember(0);
    if (args->numMembers() == 1) {
      // replace IS_NULL(x) function call with `x == null`
      return createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_EQ,
                                      args->getMemberUnchecked(0), createNodeValueNull());
    }
#if 0
  } else if (func->name == "LIKE") {
    // optimize a LIKE(x, y) into a plain x == y or a range scan in case the
    // search is case-sensitive and the pattern is either a full match or a
    // left-most prefix

    // this is desirable in 99.999% of all cases, but would cause the following incompatibilities:
    // - the AQL LIKE function will implicitly cast its operands to strings, whereas
    //   operator == in AQL will not do this. So LIKE(1, '1') would behave differently
    //   when executed via the AQL LIKE function or via 1 == '1'
    // - for left-most prefix searches (e.g. LIKE(text, 'abc%')) we need to determine
    //   the upper bound for the range scan. This is trivial for ASCII search patterns
    //   (e.g. 'abc\0xff0xff0xff...' should be big enough to include everything).
    //   But it is unclear how to achieve the upper bound for an arbitrary multi-byte
    //   character and, more grave, when using an arbitrary ICU collation where
    //   characters may be sorted differently
    // thus turned off for now, and let for future optimizations

    bool caseInsensitive = false; // this is the default behavior of LIKE
    auto args = node->getMember(0);
    if (args->numMembers() >= 3) {
      caseInsensitive = true; // we have 3 arguments, set case-sensitive to false now
      auto caseArg = args->getMember(2);
      if (caseArg->isConstant()) {
        // ok, we can figure out at compile time if the parameter is true or false
        caseInsensitive = caseArg->isTrue();
      }
    }

    auto patternArg = args->getMember(1);

    if (!caseInsensitive && patternArg->isStringValue()) {
      // optimization only possible for case-sensitive LIKE
      std::string unescapedPattern;
      bool wildcardFound;
      bool wildcardIsLastChar;
      std::tie(wildcardFound, wildcardIsLastChar) = AqlFunctionsInternalCache::inspectLikePattern(unescapedPattern, patternArg->getStringValue(), patternArg->getStringLength());

      if (!wildcardFound) {
        TRI_ASSERT(!wildcardIsLastChar);

        // can turn LIKE into ==
        char const* p = _resources.registerString(unescapedPattern.data(), unescapedPattern.size());
        AstNode* pattern = createNodeValueString(p, unescapedPattern.size());

        return createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_EQ, args->getMember(0), pattern);
      } else if (!unescapedPattern.empty()) {
        // can turn LIKE into >= && <=
        char const* p = _resources.registerString(unescapedPattern.data(), unescapedPattern.size());
        AstNode* pattern = createNodeValueString(p, unescapedPattern.size());
        AstNode* lhs = createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_GE, args->getMember(0), pattern);

        // add a new end character that is expected to sort "higher" than anything else
        char const* v = "\xef\xbf\xbf";
        unescapedPattern.append(&v[0], 3);
        p = _resources.registerString(unescapedPattern.data(), unescapedPattern.size());
        pattern = createNodeValueString(p, unescapedPattern.size());
        AstNode* rhs = createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_LE, args->getMember(0), pattern);

        AstNode* op = createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_AND, lhs, rhs);
        if (wildcardIsLastChar) {
          // replace LIKE with >= && <=
          return op;
        }
        // add >= && <=, but keep LIKE in place
        return createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_AND, op, node);
      }
    }
#endif
  }

  if (!func->hasFlag(Function::Flags::Deterministic)) {
    // non-deterministic function
    return node;
  }

  if (!node->getMember(0)->isConstant()) {
    // arguments to function call are not constant
    return node;
  }

  if (node->willUseV8()) {
    // if the expression is going to use V8 internally, we do not
    // bother to optimize it here
    return node;
  }

  Expression exp(this, node);
  FixedVarExpressionContext context(trx, _query, aqlFunctionsInternalCache);
  bool mustDestroy;

  AqlValue a = exp.execute(&context, mustDestroy);
  AqlValueGuard guard(a, mustDestroy);

  AqlValueMaterializer materializer(trx.transactionContextPtr()->getVPackOptions());
  return nodeFromVPack(materializer.slice(a, false), true);
}

/// @brief optimizes a reference to a variable
/// references are replaced with constants if possible
AstNode* Ast::optimizeReference(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_REFERENCE);

  auto variable = static_cast<Variable*>(node->getData());

  if (variable == nullptr) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  // constant propagation
  if (variable->constValue() == nullptr) {
    return node;
  }

  if (node->hasFlag(FLAG_KEEP_VARIABLENAME)) {
    // this is a reference to a variable name, not a reference to the result
    // this can be happen for variables that are specified in the COLLECT...KEEP
    // clause
    return node;
  }

  return static_cast<AstNode*>(variable->constValue());
}

/// @brief optimizes indexed access, e.g. a[0] or a['foo']
AstNode* Ast::optimizeIndexedAccess(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_INDEXED_ACCESS);
  TRI_ASSERT(node->numMembers() == 2);

  auto index = node->getMember(1);

  if (index->isConstant() && index->type == NODE_TYPE_VALUE &&
      index->value.type == VALUE_TYPE_STRING) {
    // found a string value (e.g. a['foo']). now turn this into
    // an attribute access (e.g. a.foo) in order to make the node qualify
    // for being turned into an index range later
    arangodb::velocypack::StringRef indexValue(index->getStringValue(),
                                               index->getStringLength());

    if (!indexValue.empty() && (indexValue[0] < '0' || indexValue[0] > '9')) {
      // we have to be careful with numeric values here...
      // e.g. array['0'] is not the same as array.0 but must remain a['0'] or
      // (a[0])
      return createNodeAttributeAccess(node->getMember(0), index->getStringValue(),
                                       index->getStringLength());
    }
  }

  // can't optimize when we get here
  return node;
}

/// @brief optimizes the LET statement
AstNode* Ast::optimizeLet(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_LET);
  TRI_ASSERT(node->numMembers() >= 2);

  AstNode* variable = node->getMember(0);
  AstNode* expression = node->getMember(1);

  bool const hasCondition = (node->numMembers() > 2);

  auto v = static_cast<Variable*>(variable->getData());
  TRI_ASSERT(v != nullptr);

  if (!hasCondition && expression->isConstant()) {
    // if the expression assigned to the LET variable is constant, we'll store
    // a pointer to the const value in the variable
    // further optimizations can then use this pointer and optimize further,
    // e.g.
    // LET a = 1 LET b = a + 1, c = b + a can be optimized to LET a = 1 LET b =
    // 2 LET c = 4
    v->constValue(static_cast<void*>(expression));
  }

  return node;
}

/// @brief optimizes the FILTER statement
AstNode* Ast::optimizeFilter(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_FILTER);
  TRI_ASSERT(node->numMembers() == 1);

  AstNode* expression = node->getMember(0);

  if (expression == nullptr || !expression->isDeterministic()) {
    return node;
  }

  if (expression->isTrue()) {
    // optimize away the filter if it is always true
    return createNodeFilter(createNodeValueBool(true));
  }

  if (expression->isFalse()) {
    // optimize away the filter if it is always false
    return createNodeFilter(createNodeValueBool(false));
  }

  return node;
}

/// @brief optimizes the FOR statement
/// no real optimizations are done here, but we do an early check if the
/// FOR loop operand is actually a list
AstNode* Ast::optimizeFor(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_FOR);
  TRI_ASSERT(node->numMembers() == 3);

  AstNode* expression = node->getMember(1);

  if (expression == nullptr) {
    return node;
  }

  if (expression->isConstant() && expression->type != NODE_TYPE_ARRAY) {
    // right-hand operand to FOR statement is no array
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_QUERY_ARRAY_EXPECTED,
        std::string("collection or ") + TRI_errno_string(TRI_ERROR_QUERY_ARRAY_EXPECTED) +
            std::string(" as operand to FOR loop; you specified type '") +
            expression->getValueTypeString() + std::string("' with content '") +
            expression->toString() + std::string("'"));
  }

  // no real optimizations will be done here
  return node;
}

/// @brief optimizes an object literal or an object expression
AstNode* Ast::optimizeObject(AstNode* node) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_OBJECT);

  size_t const n = node->numMembers();

  // only useful to check when there are 2 or more keys
  if (n < 2) {
    // no need to check for uniqueless later
    node->setFlag(DETERMINED_CHECKUNIQUENESS);
    return node;
  }

  std::unordered_set<std::string> keys;

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    if (member->type == NODE_TYPE_OBJECT_ELEMENT) {
      // constant key
      if (!keys.emplace(member->getString()).second) {
        // duplicate key
        node->setFlag(DETERMINED_CHECKUNIQUENESS, VALUE_CHECKUNIQUENESS);
        return node;
      }
    } else if (member->type == NODE_TYPE_CALCULATED_OBJECT_ELEMENT) {
      // dynamic key... we don't know the key yet, so there's no
      // way around check it at runtime later
      node->setFlag(DETERMINED_CHECKUNIQUENESS, VALUE_CHECKUNIQUENESS);
      return node;
    }
  }

  // no real optimizations will be done here, but we simply
  // note that we don't need to perform any uniqueness checks later
  node->setFlag(DETERMINED_CHECKUNIQUENESS);
  return node;
}

/// @brief create an AST node from vpack
/// if copyStringValues is `true`, then string values will be copied and will
/// be freed with the query afterwards. when set to `false`, string values
/// will not be copied and not freed by the query. the caller needs to make
/// sure then that string values are valid through the query lifetime.
AstNode* Ast::nodeFromVPack(VPackSlice const& slice, bool copyStringValues) {
  if (slice.isBoolean()) {
    return createNodeValueBool(slice.getBoolean());
  }

  if (slice.isNumber()) {
    if (slice.isSmallInt() || slice.isInt()) {
      // integer value
      return createNodeValueInt(slice.getIntUnchecked());
    }
    if (slice.isUInt()) {
      // check if we can safely convert the value from unsigned to signed
      // without data loss
      uint64_t v = slice.getUIntUnchecked();
      if (v <= uint64_t(INT64_MAX)) {
        return createNodeValueInt(static_cast<int64_t>(v));
      }
      // fall-through to floating point conversion
    }
    // floating point value
    return createNodeValueDouble(slice.getNumber<double>());
  }

  if (slice.isString()) {
    VPackValueLength length;
    char const* p = slice.getStringUnchecked(length);

    if (copyStringValues) {
      // we must copy string values!
      p = _resources.registerString(p, static_cast<size_t>(length));
    }
    // we can get away without copying string values
    return createNodeValueString(p, static_cast<size_t>(length));
  }

  if (slice.isArray()) {
    VPackArrayIterator it(slice);
    auto node = createNodeArray(static_cast<size_t>(it.size()));

    while (it.valid()) {
      node->addMember(nodeFromVPack(it.value(), copyStringValues));
      it.next();
    }

    node->setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
    node->setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    node->setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);

    return node;
  }

  if (slice.isObject()) {
    VPackObjectIterator it(slice, true);

    auto node = createNodeObject();
    node->members.reserve(static_cast<size_t>(it.size()));

    while (it.valid()) {
      auto current = (*it);
      VPackValueLength nameLength;
      char const* attributeName = current.key.getString(nameLength);

      if (copyStringValues) {
        // create a copy of the string value
        attributeName =
            _resources.registerString(attributeName, static_cast<size_t>(nameLength));
      }

      node->addMember(
          createNodeObjectElement(attributeName, static_cast<size_t>(nameLength),
                                  nodeFromVPack(current.value, copyStringValues)));
      it.next();
    }

    node->setFlag(DETERMINED_CONSTANT, VALUE_CONSTANT);
    node->setFlag(DETERMINED_SIMPLE, VALUE_SIMPLE);
    node->setFlag(DETERMINED_RUNONDBSERVER, VALUE_RUNONDBSERVER);

    return node;
  }

  return createNodeValueNull();
}

/// @brief resolve an attribute access
AstNode const* Ast::resolveConstAttributeAccess(AstNode const* node, bool& isValid) {
  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == NODE_TYPE_ATTRIBUTE_ACCESS);
  AstNode const* original = node;

  ::arangodb::containers::SmallVector<arangodb::velocypack::StringRef>::allocator_type::arena_type a;
  ::arangodb::containers::SmallVector<arangodb::velocypack::StringRef> attributeNames{a};

  while (node->type == NODE_TYPE_ATTRIBUTE_ACCESS) {
    attributeNames.push_back(node->getStringRef());
    node = node->getMember(0);
  }

  size_t which = attributeNames.size();
  TRI_ASSERT(which > 0);

  while (which > 0) {
    if (node->type == NODE_TYPE_PARAMETER) {
      isValid = true;
      return original;
    }

    TRI_ASSERT(node->type == NODE_TYPE_VALUE || node->type == NODE_TYPE_ARRAY ||
               node->type == NODE_TYPE_OBJECT);

    bool found = false;

    if (node->type == NODE_TYPE_OBJECT) {
      TRI_ASSERT(which > 0);
      arangodb::velocypack::StringRef const& attributeName = attributeNames[which - 1];
      --which;

      size_t const n = node->numMembers();
      for (size_t i = 0; i < n; ++i) {
        auto member = node->getMemberUnchecked(i);

        if (member->type == NODE_TYPE_OBJECT_ELEMENT &&
            arangodb::velocypack::StringRef(member->getStringValue(),
                                            member->getStringLength()) == attributeName) {
          // found the attribute
          node = member->getMember(0);
          if (which == 0) {
            // we found what we looked for
            isValid = true;
            return node;
          }
          // we found the correct attribute but there is now an attribute
          // access on the result
          found = true;
          break;
        }
      }
    }

    if (!found) {
      break;
    }
  }

  // attribute not found or non-array
  isValid = false;
  return nullptr;
}

AstNode const* Ast::resolveConstAttributeAccess(AstNode const* node) {
  TRI_ASSERT(node != nullptr);

  bool isValid;
  node = resolveConstAttributeAccess(node, isValid);
  if (isValid) {
    return node;
  }
  // attribute not found or non-array
  return createNodeValueNull();
}

/// @brief traverse the AST, using pre- and post-order visitors
AstNode* Ast::traverseAndModify(AstNode* node,
                                std::function<bool(AstNode const*)> const& preVisitor,
                                std::function<AstNode*(AstNode*)> const& visitor,
                                std::function<void(AstNode const*)> const& postVisitor) {
  if (node == nullptr) {
    return nullptr;
  }

  if (!preVisitor(node)) {
    return node;
  }

  size_t const n = node->numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    if (member != nullptr) {
      AstNode* result = traverseAndModify(member, preVisitor, visitor, postVisitor);

      if (result != member) {
        TEMPORARILY_UNLOCK_NODE(node);
        TRI_ASSERT(node != nullptr);
        node->changeMember(i, result);
      }
    }
  }

  auto result = visitor(node);
  postVisitor(node);
  return result;
}

/// @brief traverse the AST, using a depth-first visitor
/// Note that the starting node is not replaced!
AstNode* Ast::traverseAndModify(AstNode* node,
                                std::function<AstNode*(AstNode*)> const& visitor) {
  if (node == nullptr) {
    return nullptr;
  }

  size_t const n = node->numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    if (member != nullptr) {
      AstNode* result = traverseAndModify(member, visitor);

      if (result != member) {
        TEMPORARILY_UNLOCK_NODE(node);  // TODO change so we can replace instead
        node->changeMember(i, result);
      }
    }
  }

  return visitor(node);
}

/// @brief traverse the AST, using pre- and post-order visitors
void Ast::traverseReadOnly(AstNode const* node,
                           std::function<bool(AstNode const*)> const& preVisitor,
                           std::function<void(AstNode const*)> const& postVisitor) {
  if (node == nullptr) {
    return;
  }

  if (!preVisitor(node)) {
    return;
  }
  size_t const n = node->numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    if (member != nullptr) {
      traverseReadOnly(member, preVisitor, postVisitor);
    }
  }

  postVisitor(node);
}

/// @brief traverse the AST using a visitor depth-first, with const nodes
void Ast::traverseReadOnly(AstNode const* node,
                           std::function<void(AstNode const*)> const& visitor) {
  if (node == nullptr) {
    return;
  }

  size_t const n = node->numMembers();

  for (size_t i = 0; i < n; ++i) {
    auto member = node->getMemberUnchecked(i);

    if (member != nullptr) {
      traverseReadOnly(member, visitor);
    }
  }

  visitor(node);
}

/// @brief normalize a function name
std::pair<std::string, bool> Ast::normalizeFunctionName(char const* name, size_t length) {
  TRI_ASSERT(name != nullptr);

  std::string functionName(name, length);
  // convert name to upper case
  std::transform(functionName.begin(), functionName.end(), functionName.begin(), ::toupper);

  return std::make_pair(std::move(functionName), functionName.find(':') == std::string::npos);
}

/// @brief create a node of the specified type
AstNode* Ast::createNode(AstNodeType type) {
  return _resources.registerNode(type);
}

/// @brief validate the name of the given datasource
/// in case validation fails, will throw an exception
void Ast::validateDataSourceName(arangodb::velocypack::StringRef const& name,
                                 bool validateStrict) {
  // common validation
  if (name.empty() ||
      (validateStrict &&
       !TRI_vocbase_t::IsAllowedName(
           true, arangodb::velocypack::StringRef(name.data(), name.size())))) {
    // will throw    
    std::string errorMessage(TRI_errno_string(TRI_ERROR_ARANGO_ILLEGAL_NAME));
    errorMessage.append(": ");
    errorMessage.append(name.data(), name.size());

    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_ILLEGAL_NAME, errorMessage);
  }
}

/// @brief create an AST collection node
/// private function, does no validation
AstNode* Ast::createNodeCollectionNoValidation(arangodb::velocypack::StringRef const& name,
                                               AccessMode::Type accessType) {
  if (ServerState::instance()->isCoordinator()) {
    auto& ci = _query.vocbase().server().getFeature<ClusterFeature>().clusterInfo();
    // We want to tolerate that a collection name is given here
    // which does not exist, if only for some unit tests:
    auto coll = ci.getCollectionNT(_query.vocbase().name(), name.toString());
    if (coll != nullptr && coll->isSmart()) {
      // add names of underlying smart-edge collections
      for (auto const& n : coll->realNames()) {
        _query.collections().add(n, accessType, Collection::Hint::Collection);
      }
    }
  }

  AstNode* node = createNode(NODE_TYPE_COLLECTION);
  node->setStringValue(name.data(), name.size());

  return node;
}

void Ast::extractCollectionsFromGraph(AstNode const* graphNode) {
  TRI_ASSERT(graphNode != nullptr);
  if (graphNode->type == NODE_TYPE_VALUE) {
    TRI_ASSERT(graphNode->isStringValue());
    std::string graphName = graphNode->getString();
    auto graphLookupRes = _query.lookupGraphByName(graphName);
    if (graphLookupRes.fail()) {
      THROW_ARANGO_EXCEPTION(graphLookupRes.result());
    }

    TRI_ASSERT(graphLookupRes.ok());

    auto const& graph = graphLookupRes.get();
    for (const auto& n : graph->vertexCollections()) {
      _query.collections().add(n, AccessMode::Type::READ, Collection::Hint::Collection);
    }

    auto const& eColls = graph->edgeCollections();

    for (const auto& n : eColls) {
      _query.collections().add(n, AccessMode::Type::READ, Collection::Hint::Collection);
    }

    if (ServerState::instance()->isCoordinator()) {
      auto& ci = _query.vocbase().server().getFeature<ClusterFeature>().clusterInfo();

      for (const auto& n : eColls) {
        auto c = ci.getCollection(_query.vocbase().name(), n);
        for (auto const& name : c->realNames()) {
          _query.collections().add(name, AccessMode::Type::READ, Collection::Hint::Collection);
        }
      }
    }
  }
}

VariableGenerator* Ast::variables() { return &_variables; }
VariableGenerator const* Ast::variables() const { return &_variables; }

AstNode const* Ast::root() const { return _root; }

void Ast::startSubQuery() {
  // insert a new root node
  AstNodeType type;

  if (_queries.empty()) {
    // root node of query
    type = NODE_TYPE_ROOT;
  } else {
    // sub query node
    type = NODE_TYPE_SUBQUERY;
  }

  auto root = createNode(type);

  // save the root node
  _queries.emplace_back(root);

  // set the current root node if everything went well
  _root = root;
}

AstNode* Ast::endSubQuery() {
  // get the current root node
  AstNode* root = _queries.back();
  // remove it from the stack
  _queries.pop_back();

  // set root node to previous root node
  _root = _queries.back();

  // return the root node we just popped from the stack
  return root;
}

bool Ast::isInSubQuery() const { return (_queries.size() > 1); }
std::unordered_set<std::string> Ast::bindParameters() const {
  return std::unordered_set<std::string>(_bindParameters);
}

Scopes* Ast::scopes() { return &_scopes; }
void Ast::addWriteCollection(AstNode const* node, bool isExclusiveAccess) {
  TRI_ASSERT(node->type == NODE_TYPE_COLLECTION || node->type == NODE_TYPE_PARAMETER_DATASOURCE);

  _writeCollections.emplace_back(node, isExclusiveAccess);
}

bool Ast::functionsMayAccessDocuments() const {
  return _functionsMayAccessDocuments;
}

bool Ast::containsTraversal() const noexcept { return _containsTraversal; }

bool Ast::containsModificationNode() const noexcept { return _containsModificationNode; }

void Ast::setContainsModificationNode() noexcept {
  _containsModificationNode = true;
}

void Ast::setContainsParallelNode() noexcept {
#ifdef USE_ENTERPRISE
  _containsParallelNode = true;
#endif
}

bool Ast::willUseV8() const noexcept {
  return _willUseV8;
}

void Ast::setWillUseV8() noexcept {
  _willUseV8 = true;
}

AstNode* Ast::createNodeAttributeAccess(AstNode const* node,
                                        std::vector<basics::AttributeName> const& attrs) {
  std::vector<std::string> vec;  // change to std::string_view once available
  std::transform(attrs.begin(), attrs.end(), std::back_inserter(vec),
                 [](basics::AttributeName const& a) { return a.name; });
  return createNodeAttributeAccess(node, vec);
}

AstNode* Ast::createNodeFunctionCall(char const* functionName, AstNode const* arguments) {
  return createNodeFunctionCall(functionName, strlen(functionName), arguments);
}

AstNode* Ast::createNodeReadAll() {
  return createNode(NODE_TYPE_READ_ALL);
}