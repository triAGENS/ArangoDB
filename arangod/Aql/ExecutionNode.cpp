////////////////////////////////////////////////////////////////////////////////
/// @brief Infrastructure for ExecutionPlans
///
/// @file arangod/Aql/ExecutionNode.cpp
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Copyright 2014, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Aql/ExecutionNode.h"
#include "Aql/Collection.h"
#include "Aql/WalkerWorker.h"
#include "Aql/Ast.h"

using namespace triagens::basics;
using namespace triagens::aql;

const static bool Optional = true;
// -----------------------------------------------------------------------------
// --SECTION--                                             static initialization
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief type names
////////////////////////////////////////////////////////////////////////////////

std::unordered_map<int, std::string const> const ExecutionNode::TypeNames{ 
  { static_cast<int>(ILLEGAL),                      "ExecutionNode (abstract)" },
  { static_cast<int>(SINGLETON),                    "SingletonNode" },
  { static_cast<int>(ENUMERATE_COLLECTION),         "EnumerateCollectionNode" },
  { static_cast<int>(ENUMERATE_LIST),               "EnumerateListNode" },
  { static_cast<int>(INDEX_RANGE),                  "IndexRangeNode" },
  { static_cast<int>(LIMIT),                        "LimitNode" },
  { static_cast<int>(CALCULATION),                  "CalculationNode" },
  { static_cast<int>(SUBQUERY),                     "SubqueryNode" },
  { static_cast<int>(FILTER),                       "FilterNode" },
  { static_cast<int>(SORT),                         "SortNode" },
  { static_cast<int>(AGGREGATE),                    "AggregateNode" },
  { static_cast<int>(RETURN),                       "ReturnNode" },
  { static_cast<int>(REMOVE),                       "RemoveNode" },
  { static_cast<int>(INSERT),                       "InsertNode" },
  { static_cast<int>(UPDATE),                       "UpdateNode" },
  { static_cast<int>(REPLACE),                      "ReplaceNode" },
  { static_cast<int>(NORESULTS),                    "NoResultsNode" }
};
          
// -----------------------------------------------------------------------------
// --SECTION--                                          methods of ExecutionNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the type name of the node
////////////////////////////////////////////////////////////////////////////////

const std::string& ExecutionNode::getTypeString () const {
  auto it = TypeNames.find(static_cast<int>(getType()));
  if (it != TypeNames.end()) {
    return (*it).second;
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, "missing type in TypeNames");
}

void ExecutionNode::validateType (int type) {
  auto it = TypeNames.find(static_cast<int>(type));
  if (it == TypeNames.end()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, "unknown TypeID");
  }
}

ExecutionNode* ExecutionNode::fromJsonFactory (Ast* ast,
                                               Json const& oneNode) {
  auto JsonString = oneNode.toString();

  int nodeTypeID = JsonHelper::getNumericValue<int>(oneNode.json(), "typeID", 0);
  validateType(nodeTypeID);

  NodeType nodeType = (NodeType) nodeTypeID;
  switch (nodeType) {
    case SINGLETON:
      return new SingletonNode(ast, oneNode);
    case ENUMERATE_COLLECTION:
      return new EnumerateCollectionNode(ast, oneNode);
    case ENUMERATE_LIST:
      return new EnumerateListNode(ast, oneNode);
    case FILTER:
      return new FilterNode(ast, oneNode);
    case LIMIT:
      return new LimitNode(ast, oneNode);
    case CALCULATION:
      return new CalculationNode(ast, oneNode);
    case SUBQUERY: 
      return new SubqueryNode(ast, oneNode);
    case SORT: {
      Json jsonElements = oneNode.get("elements");
      if (! jsonElements.isList()){
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "unexpected value for SortNode elements");
      }
      size_t len = jsonElements.size();
      std::vector<std::pair<Variable const*, bool>> elements;
      elements.reserve(len);
      for (size_t i = 0; i < len; i++) {
        Json oneJsonElement = jsonElements.at(i);
        bool ascending = JsonHelper::getBooleanValue(oneJsonElement.json(), "ascending", false);
        Variable *v = varFromJson(ast, oneJsonElement, "inVariable");
        elements.push_back(std::make_pair(v, ascending));
      }

      return new SortNode(ast, oneNode, elements);
    }
    case AGGREGATE: {

      Variable *outVariable = varFromJson(ast, oneNode, "outVariable", Optional);

      Json jsonAaggregates = oneNode.get("aggregates");
      if (! jsonAaggregates.isList()){
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, "missing node type in valueTypeNames"); 
      }

      int len = jsonAaggregates.size();
      std::vector<std::pair<Variable const*, Variable const*>> aggregateVariables;

      aggregateVariables.reserve(len);
      for (int i = 0; i < len; i++) {
        Json oneJsonAggregate = jsonAaggregates.at(i);
        Variable* outVariable = varFromJson(ast, oneJsonAggregate, "outVariable");
        Variable* inVariable =  varFromJson(ast, oneJsonAggregate, "inVariable");

        aggregateVariables.push_back(std::make_pair(outVariable, inVariable));
      }

      return new AggregateNode(ast,
                              oneNode,
                              outVariable,
                              ast->variables()->variables(false),
                              aggregateVariables);
    }
    case INSERT:
      return new InsertNode(ast, oneNode);
    case REMOVE:
      return new RemoveNode(ast, oneNode);
    case REPLACE:
      return new ReplaceNode(ast, oneNode);
    case UPDATE:
      return new UpdateNode(ast, oneNode);
    case RETURN:
      return new ReturnNode(ast, oneNode);
    case NORESULTS:
      return new NoResultsNode(ast, oneNode);
    case INDEX_RANGE:
      return new IndexRangeNode(ast, oneNode);
    case INTERSECTION:
    case LOOKUP_JOIN:
    case MERGE_JOIN:
    case LOOKUP_INDEX_UNIQUE:
    case LOOKUP_INDEX_RANGE:
    case LOOKUP_FULL_COLLECTION:
    case CONCATENATION:
    case MERGE:
    case REMOTE: {
      // TODO: handle these types of nodes
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED, "unhandled node type");
    }

    case ILLEGAL: {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "invalid node type");
    }
  }
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create an ExecutionNode from JSON
////////////////////////////////////////////////////////////////////////////////

ExecutionNode::ExecutionNode (triagens::basics::Json const& json) 
  : ExecutionNode(JsonHelper::getNumericValue<size_t>(json.json(), "id", 0),
                  JsonHelper::getNumericValue<double>(json.json(), "estimatedCost", 0.0)) { 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, export an ExecutionNode to JSON
////////////////////////////////////////////////////////////////////////////////

Json ExecutionNode::toJson (TRI_memory_zone_t* zone,
                            bool verbose) const {
  Json json;
  Json nodes;
  try {
    nodes = Json(Json::List, 10);
    toJsonHelper(nodes, zone, verbose);
    json = Json(Json::Array, 1)
             ("nodes", nodes);
  }
  catch (std::exception& e) {
    return Json();
  }

  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief convert to a string, basically for debugging purposes
////////////////////////////////////////////////////////////////////////////////

void ExecutionNode::appendAsString (std::string& st, int indent) {
  for (int i = 0; i < indent; i++) {
    st.push_back(' ');
  }
  
  st.push_back('<');
  st.append(getTypeString());
  if (_dependencies.size() != 0) {
    st.push_back('\n');
    for (size_t i = 0; i < _dependencies.size(); i++) {
      _dependencies[i]->appendAsString(st, indent + 2);
      if (i != _dependencies.size() - 1) {
        st.push_back(',');
      }
      else {
        st.push_back(' ');
      }
    }
  }
  st.push_back('>');
}

////////////////////////////////////////////////////////////////////////////////
/// @brief functionality to walk an execution plan recursively
////////////////////////////////////////////////////////////////////////////////

void ExecutionNode::walk (WalkerWorker<ExecutionNode>* worker) {
  // Only do every node exactly once:
  if (worker->done(this)) {
    return;
  }

  worker->before(this);
  
  // Now the children in their natural order:
  for (auto it = _dependencies.begin();
            it != _dependencies.end(); 
            ++it) {
    (*it)->walk(worker);
  }
  
  // Now handle a subquery:
  if (getType() == SUBQUERY) {
    auto p = static_cast<SubqueryNode*>(this);
    if (worker->enterSubquery(this, p->getSubquery())) {
      p->getSubquery()->walk(worker);
      worker->leaveSubquery(this, p->getSubquery());
    }
  }

  worker->after(this);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 protected methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief factory for (optional) variables from json.
////////////////////////////////////////////////////////////////////////////////

Variable* ExecutionNode::varFromJson (Ast* ast,
                                      triagens::basics::Json const& base,
                                      const char *variableName,
                                      bool optional) {
  Json variableJson = base.get(variableName);

  if (variableJson.isEmpty()) {
    if (optional) {
      return nullptr;
    }
    else {
      std::string msg;
      msg += "Mandatory variable \"" + std::string(variableName) + "\"not found.";
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg.c_str());
    }
  }
  else {
    return ast->variables()->createVariable(variableJson);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJsonHelper, for a generic node
////////////////////////////////////////////////////////////////////////////////

Json ExecutionNode::toJsonHelperGeneric (triagens::basics::Json& nodes,
                                         TRI_memory_zone_t* zone,
                                         bool verbose) const {
  size_t const n = _dependencies.size();
  for (size_t i = 0; i < n; i++) {
    _dependencies[i]->toJsonHelper(nodes, zone, verbose);
  }

  Json json;
  json = Json(Json::Array, 2)
              ("type", Json(getTypeString()));
  if (verbose) {
    json ("typeID", Json(static_cast<int>(getType())));
  }
  Json deps(Json::List, n);
  for (size_t i = 0; i < n; i++) {
    deps(Json(static_cast<double>(_dependencies[i]->id())));
  }
  json("dependencies", deps);
  Json parents(Json::List, _parents.size());
  for (size_t i = 0; i < _parents.size(); i++) {
    parents(Json(static_cast<double>(_parents[i]->id())));
  }
  if (verbose) {
    json("parents", parents);
  }
  json("id", Json(static_cast<double>(id())));

  if (_estimatedCost != 0.0) {
    json("estimatedCost", Json(_estimatedCost));
  }
  return json;
}

// -----------------------------------------------------------------------------
// --SECTION--                                          methods of SingletonNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for SingletonNode
////////////////////////////////////////////////////////////////////////////////

SingletonNode::SingletonNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base) {
}

void SingletonNode::toJsonHelper (triagens::basics::Json& nodes,
                                  TRI_memory_zone_t* zone,
                                  bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                methods of EnumerateCollectionNode
// -----------------------------------------------------------------------------

EnumerateCollectionNode::EnumerateCollectionNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base),
    _vocbase(ast->query()->vocbase()),
    _collection(ast->query()->collections()->add(JsonHelper::getStringValue(base.json(), "collection", ""), TRI_TRANSACTION_READ)),
    _outVariable(varFromJson(ast, base, "outVariable")) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for EnumerateCollectionNode
////////////////////////////////////////////////////////////////////////////////

void EnumerateCollectionNode::toJsonHelper (triagens::basics::Json& nodes,
                                            TRI_memory_zone_t* zone,
                                            bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method

  if (json.isEmpty()) {
    return;
  }

  // Now put info about vocbase and cid in there
  json("database", Json(_vocbase->_name))
      ("collection", Json(_collection->name))
      ("outVariable", _outVariable->toJson());

  // And add it:
  nodes(json);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get vector of indexes with fields <attrs> 
////////////////////////////////////////////////////////////////////////////////

std::vector<TRI_index_t*> EnumerateCollectionNode::getIndexes (vector<std::string> attrs) const {
  std::vector<TRI_index_t*> out;
  TRI_document_collection_t* document = _collection->documentCollection();
  size_t const n = document->_allIndexes._length;
  
  for (size_t i = 0; i < n; ++i) {
    TRI_index_t* idx = static_cast<TRI_index_t*>(document->_allIndexes._buffer[i]);

    size_t seen = 0;
    for (size_t j = 0; j < idx->_fields._length; j++) {
      bool found = false;
      for (size_t k = 0; k < attrs.size(); k++){
        if(std::string(idx->_fields._buffer[j]) == attrs[k]) {
          found = true;
          break;
        }
      }
      if (found) {
        seen++;
      }
      else {
        break;
      }
    }

    if (((idx->_type == TRI_IDX_TYPE_HASH_INDEX) && seen == idx->_fields._length ) 
        || ((idx->_type == TRI_IDX_TYPE_SKIPLIST_INDEX) && seen > 0 )) {
      // all fields equal 
      out.push_back(idx);
    }
  }
  return out;
}

// -----------------------------------------------------------------------------
// --SECTION--                                      methods of EnumerateListNode
// -----------------------------------------------------------------------------

EnumerateListNode::EnumerateListNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base),
    _inVariable(varFromJson(ast, base, "inVariable")),
    _outVariable(varFromJson(ast, base, "outVariable")) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for EnumerateListNode
////////////////////////////////////////////////////////////////////////////////

void EnumerateListNode::toJsonHelper (triagens::basics::Json& nodes,
                                      TRI_memory_zone_t* zone,
                                      bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }
  json("inVariable",  _inVariable->toJson())
      ("outVariable", _outVariable->toJson());

  // And add it:
  nodes(json);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for IndexRangeNode
////////////////////////////////////////////////////////////////////////////////

void IndexRangeNode::toJsonHelper (triagens::basics::Json& nodes,
                                   TRI_memory_zone_t* zone,
                                   bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  
  // call base class method

  if (json.isEmpty()) {
    return;
  }

  // put together the range info . . .
  Json ranges(Json::List);

  for (auto x : _ranges) {
    ranges(x->toJson());
  }
      
  // Now put info about vocbase and cid in there
  json("database", Json(_vocbase->_name))
      ("collection", Json(_collection->name))
      ("outVariable", _outVariable->toJson())
      ("ranges", ranges);
  
  TRI_json_t* idxJson = _index->json(_index);
  if (idxJson != nullptr) {
    try {
      json.set("index", Json(TRI_UNKNOWN_MEM_ZONE, TRI_CopyJson(TRI_UNKNOWN_MEM_ZONE, idxJson)));
    }
    catch (...) {
    }
    TRI_Free(TRI_CORE_MEM_ZONE, idxJson);
  }

  // And add it:
  nodes(json);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor for IndexRangeNode from Json
////////////////////////////////////////////////////////////////////////////////

IndexRangeNode::IndexRangeNode (Ast* ast, basics::Json const& json)
  : ExecutionNode(json),
    _vocbase(ast->query()->vocbase()),
    _collection(ast->query()->collections()->add(JsonHelper::getStringValue(json.json(), 
            "collection"), TRI_TRANSACTION_READ)),
    _outVariable(varFromJson(ast, json, "outVariable")), _ranges() {
      
  for(size_t i = 0; i < json.size(); i++){ //loop over the ranges . . .
    _ranges.push_back(new RangeInfo(json.at(i)));
  }
  
  // now the index . . . 
  // TODO the following could be a constructor method for
  // an Index object when these are actually used
  auto index = JsonHelper::getArray(json.json(), "index");
  auto iid = JsonHelper::getArrayElement(index, "id");
  _index = TRI_LookupIndex(_collection->documentCollection(), 
      JsonHelper::getNumericValue<TRI_idx_iid_t>(iid, 0));
}

// -----------------------------------------------------------------------------
// --SECTION--                                              methods of LimitNode
// -----------------------------------------------------------------------------

LimitNode::LimitNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base) {
  _offset = JsonHelper::getNumericValue<double>(base.json(), "offset", 0.0);
  _limit = JsonHelper::getNumericValue<double>(base.json(), "limit", 0.0);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for LimitNode
////////////////////////////////////////////////////////////////////////////////

void LimitNode::toJsonHelper (triagens::basics::Json& nodes,
                              TRI_memory_zone_t* zone,
                              bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }
  // Now put info about offset and limit in
  json("offset", Json(static_cast<double>(_offset)))
      ("limit",  Json(static_cast<double>(_limit)));

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                        methods of CalculationNode
// -----------------------------------------------------------------------------

CalculationNode::CalculationNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base),
    _expression(new Expression(ast, base)),
    _outVariable(varFromJson(ast, base, "outVariable")) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for CalculationNode
////////////////////////////////////////////////////////////////////////////////

void CalculationNode::toJsonHelper (triagens::basics::Json& nodes,
                                    TRI_memory_zone_t* zone,
                                    bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }
  
  json("expression", _expression->toJson(TRI_UNKNOWN_MEM_ZONE, verbose))
      ("outVariable", _outVariable->toJson())
      ("canThrow", Json(_expression->canThrow()));


  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                           methods of SubqueryNode
// -----------------------------------------------------------------------------

SubqueryNode::SubqueryNode (Ast* ast,
                            basics::Json const& base)
  : ExecutionNode(base),
    _subquery(nullptr),
    _outVariable(varFromJson(ast, base, "outVariable")) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for SubqueryNode
////////////////////////////////////////////////////////////////////////////////

void SubqueryNode::toJsonHelper (triagens::basics::Json& nodes,
                                 TRI_memory_zone_t* zone,
                                 bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }
  json("subquery",  _subquery->toJson(TRI_UNKNOWN_MEM_ZONE, verbose))
      ("outVariable", _outVariable->toJson());

  // And add it:
  nodes(json);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief helper struct to find all (outer) variables used in a SubqueryNode
////////////////////////////////////////////////////////////////////////////////

struct SubqueryVarUsageFinder : public WalkerWorker<ExecutionNode> {
  std::unordered_set<Variable const*> _usedLater;
  std::unordered_set<Variable const*> _valid;

  SubqueryVarUsageFinder () {
  }

  ~SubqueryVarUsageFinder () {
  }

  void before (ExecutionNode* en) {
    // Add variables used here to _usedLater:
    auto&& usedHere = en->getVariablesUsedHere();
    for (auto v : usedHere) {
      _usedLater.insert(v);
    }
  }

  void after (ExecutionNode* en) {
    // Add variables set here to _valid:
    auto&& setHere = en->getVariablesSetHere();
    for (auto v : setHere) {
      _valid.insert(v);
    }
  }

  bool enterSubquery (ExecutionNode* super, ExecutionNode* sub) {
    SubqueryVarUsageFinder subfinder;
    sub->walk(&subfinder);

    // keep track of all variables used by a (dependent) subquery
    // this is, all variables in the subqueries _usedLater that are not in _valid
     
    // create the set difference. note: cannot use std::set_difference as our sets are NOT sorted
    for (auto it = subfinder._usedLater.begin(); it != subfinder._usedLater.end(); ++it) {
      if (_valid.find(*it) != _valid.end()) {
        _usedLater.insert((*it));
      }
    }
    return false;
  }
};

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

std::vector<Variable const*> SubqueryNode::getVariablesUsedHere () const {
  SubqueryVarUsageFinder finder;
  _subquery->walk(&finder);
      
  std::vector<Variable const*> v;
  for (auto it = finder._usedLater.begin(); it != finder._usedLater.end(); ++it) {
    if (finder._valid.find(*it) == finder._valid.end()) {
      v.push_back((*it));
    }
  }
  return v;
}

// -----------------------------------------------------------------------------
// --SECTION--                                             methods of FilterNode
// -----------------------------------------------------------------------------

FilterNode::FilterNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base),
    _inVariable(varFromJson(ast, base, "inVariable")) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for FilterNode
////////////////////////////////////////////////////////////////////////////////

void FilterNode::toJsonHelper (triagens::basics::Json& nodes,
                               TRI_memory_zone_t* zone,
                               bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }
  
  json("inVariable", _inVariable->toJson());

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                               methods of SortNode
// -----------------------------------------------------------------------------

SortNode::SortNode (Ast* ast,
                    basics::Json const& base,
                    std::vector<std::pair<Variable const*, bool>> elements)
  : ExecutionNode(base),
    _elements(elements) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for SortNode
////////////////////////////////////////////////////////////////////////////////

void SortNode::toJsonHelper (triagens::basics::Json& nodes,
                             TRI_memory_zone_t* zone,
                             bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }
  Json values(Json::List, _elements.size());
  for (auto it = _elements.begin(); it != _elements.end(); ++it) {
    Json element(Json::Array);
    element("inVariable", (*it).first->toJson())
           ("ascending", Json((*it).second));
    values(element);
  }
  json("elements", values);

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                          methods of AggregateNode
// -----------------------------------------------------------------------------

AggregateNode::AggregateNode (Ast* ast,
                              basics::Json const& base,
                              Variable const* outVariable,
                              std::unordered_map<VariableId, std::string const> const& variableMap,
                              std::vector<std::pair<Variable const*, Variable const*>> aggregateVariables)
  : ExecutionNode(base),
    _aggregateVariables(aggregateVariables), 
    _outVariable(outVariable),
    _variableMap(variableMap) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for AggregateNode
////////////////////////////////////////////////////////////////////////////////

void AggregateNode::toJsonHelper (triagens::basics::Json& nodes,
                                  TRI_memory_zone_t* zone,
                                  bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }

  Json values(Json::List, _aggregateVariables.size());
  for (auto it = _aggregateVariables.begin(); it != _aggregateVariables.end(); ++it) {
    Json variable(Json::Array);
    variable("outVariable", (*it).first->toJson())
            ("inVariable", (*it).second->toJson());
    values(variable);
  }
  json("aggregates", values);

  // output variable might be empty
  if (_outVariable != nullptr) {
    json("outVariable", _outVariable->toJson());
  }

  // And add it:
  nodes(json);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

struct UserVarFinder : public WalkerWorker<ExecutionNode> {
  UserVarFinder () {};
  ~UserVarFinder () {};
  std::vector<Variable const*> userVars;

  bool enterSubquery (ExecutionNode* super, ExecutionNode* sub) {
    return false;
  }
  void before (ExecutionNode* en) {
    auto vars = en->getVariablesSetHere();
    for (auto v : vars) {
      if (v->isUserDefined()) {
        userVars.push_back(v);
      }
    }
  }
};

std::vector<Variable const*> AggregateNode::getVariablesUsedHere () const {
  std::unordered_set<Variable const*> v;
  for (auto p : _aggregateVariables) {
    v.insert(p.second);
  }
  if (_outVariable != nullptr) {
    // Here we have to find all user defined variables in this query
    // amonst our dependencies:
    UserVarFinder finder;
    auto myselfasnonconst = const_cast<AggregateNode*>(this);
    myselfasnonconst->walk(&finder);
    for (auto x : finder.userVars) {
      v.insert(x);
    }
  }
  std::vector<Variable const*> vv;
  for (auto x : v) {
    vv.push_back(x);
  }
  return vv;
}

// -----------------------------------------------------------------------------
// --SECTION--                                             methods of ReturnNode
// -----------------------------------------------------------------------------

ReturnNode::ReturnNode (Ast* ast, basics::Json const& base)
  : ExecutionNode(base),
    _inVariable(varFromJson(ast, base, "inVariable")) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for ReturnNode
////////////////////////////////////////////////////////////////////////////////

void ReturnNode::toJsonHelper (triagens::basics::Json& nodes,
                               TRI_memory_zone_t* zone,
                               bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method

  if (json.isEmpty()) {
    return;
  }
      
  json("inVariable", _inVariable->toJson());

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                  ModificationNode
// -----------------------------------------------------------------------------

ModificationNode::ModificationNode (Ast* ast,
                                    basics::Json const& base)
  : ExecutionNode(base), 
    _vocbase(ast->query()->vocbase()),
    _collection(ast->query()->collections()->add(JsonHelper::getStringValue(base.json(), "collection", ""), TRI_TRANSACTION_WRITE)),
    _options(base) {
  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(_collection != nullptr);
}

// -----------------------------------------------------------------------------
// --SECTION--                                             methods of RemoveNode
// -----------------------------------------------------------------------------

RemoveNode::RemoveNode (Ast* ast, basics::Json const& base)
  : ModificationNode(ast, base),
    _inVariable(varFromJson(ast, base, "inVariable")),
    _outVariable(varFromJson(ast, base, "outVariable", Optional)) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson
////////////////////////////////////////////////////////////////////////////////

void RemoveNode::toJsonHelper (triagens::basics::Json& nodes,
                               TRI_memory_zone_t* zone,
                               bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method

  if (json.isEmpty()) {
    return;
  }

  // Now put info about vocbase and cid in there
  json("database", Json(_vocbase->_name))
      ("collection", Json(_collection->name))
      ("inVariable", _inVariable->toJson());
  
  // output variable might be empty
  if (_outVariable != nullptr) {
    json("outVariable", _outVariable->toJson());
  }

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                             methods of InsertNode
// -----------------------------------------------------------------------------

InsertNode::InsertNode (Ast* ast, basics::Json const& base)
  : ModificationNode(ast, base),
    _inVariable(varFromJson(ast, base, "inVariable")),
    _outVariable(varFromJson(ast, base, "outVariable", Optional)) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson
////////////////////////////////////////////////////////////////////////////////

void InsertNode::toJsonHelper (triagens::basics::Json& nodes,
                               TRI_memory_zone_t* zone,
                               bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method

  if (json.isEmpty()) {
    return;
  }

  // Now put info about vocbase and cid in there
  json("database", Json(_vocbase->_name))
      ("collection", Json(_collection->name))
      ("inVariable", _inVariable->toJson());
  
  // output variable might be empty
  if (_outVariable != nullptr) {
    json("outVariable", _outVariable->toJson());
  }

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                             methods of UpdateNode
// -----------------------------------------------------------------------------

UpdateNode::UpdateNode (Ast* ast, basics::Json const& base)
  : ModificationNode(ast, base),
    _inDocVariable(varFromJson(ast, base, "inDocVariable")),
    _inKeyVariable(varFromJson(ast, base, "inKeyVariable", Optional)),
    _outVariable(varFromJson(ast, base, "outVariable", Optional)) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson
////////////////////////////////////////////////////////////////////////////////

void UpdateNode::toJsonHelper (triagens::basics::Json& nodes,
                               TRI_memory_zone_t* zone,
                               bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method

  if (json.isEmpty()) {
    return;
  }

  // Now put info about vocbase and cid in there
  json("database", Json(_vocbase->_name))
      ("collection", Json(_collection->name))
      ("inDocVariable", _inDocVariable->toJson());
  
  // inKeyVariable might be empty
  if (_inKeyVariable != nullptr) {
    json("inKeyVariable", _inKeyVariable->toJson());
  }
  
  // output variable might be empty
  if (_outVariable != nullptr) {
    json("outVariable", _outVariable->toJson());
  }

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                            methods of ReplaceNode
// -----------------------------------------------------------------------------

ReplaceNode::ReplaceNode (Ast* ast, basics::Json const& base)
  : ModificationNode(ast, base),
    _inDocVariable(varFromJson(ast, base, "inDocVariable")),
    _inKeyVariable(varFromJson(ast, base, "inKeyVariable", Optional)),
    _outVariable(varFromJson(ast, base, "outVariable", Optional)) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson
////////////////////////////////////////////////////////////////////////////////

void ReplaceNode::toJsonHelper (triagens::basics::Json& nodes,
                                TRI_memory_zone_t* zone,
                                bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method

  if (json.isEmpty()) {
    return;
  }

  // Now put info about vocbase and cid in there
  json("database", Json(_vocbase->_name))
      ("collection", Json(_collection->name))
      ("inDocVariable", _inDocVariable->toJson());
  
  // inKeyVariable might be empty
  if (_inKeyVariable != nullptr) {
    json("inKeyVariable", _inKeyVariable->toJson());
  }
  
  // output variable might be empty
  if (_outVariable != nullptr) {
    json("outVariable", _outVariable->toJson());
  }

  // And add it:
  nodes(json);
}

// -----------------------------------------------------------------------------
// --SECTION--                                          methods of NoResultsNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson, for NoResultsNode
////////////////////////////////////////////////////////////////////////////////

void NoResultsNode::toJsonHelper (triagens::basics::Json& nodes,
                                  TRI_memory_zone_t* zone,
                                  bool verbose) const {
  Json json(ExecutionNode::toJsonHelperGeneric(nodes, zone, verbose));  // call base class method
  if (json.isEmpty()) {
    return;
  }

  // And add it:
  nodes(json);
}

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:


