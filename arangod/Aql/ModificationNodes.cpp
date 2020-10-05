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

#include "ModificationNodes.h"

#include "Aql/AllRowsFetcher.h"
#include "Aql/Ast.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutorHelpers.h"
#include "Aql/Query.h"
#include "Aql/SimpleModifier.h"
#include "Aql/SingleRowFetcher.h"
#include "Aql/UpsertModifier.h"
#include "Aql/VariableGenerator.h"

using namespace arangodb::aql;

namespace arangodb {
namespace aql {

ModificationNode::ModificationNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ExecutionNode(plan, base),
      CollectionAccessingNode(plan, base),
      _options(base),
      _outVariableOld(
          Variable::varFromVPack(plan->getAst(), base, "outVariableOld", true)),
      _outVariableNew(
          Variable::varFromVPack(plan->getAst(), base, "outVariableNew", true)),
      _countStats(base.get("countStats").getBool()),
      _producesResults(base.hasKey("producesResults")
                           ? base.get("producesResults").getBool()
                           : true) {}

/// @brief toVelocyPack
void ModificationNode::toVelocyPackHelper(VPackBuilder& builder, unsigned flags,
                                          std::unordered_set<ExecutionNode const*>& seen) const {
  // call base class method
  ExecutionNode::toVelocyPackHelperGeneric(builder, flags, seen);

  // add collection information
  CollectionAccessingNode::toVelocyPack(builder, flags);

  // Now put info about vocbase and cid in there
  builder.add("countStats", VPackValue(_countStats));

  builder.add("producesResults", VPackValue(_producesResults));

  // add out variables
  if (_outVariableOld != nullptr) {
    builder.add(VPackValue("outVariableOld"));
    _outVariableOld->toVelocyPack(builder);
  }
  if (_outVariableNew != nullptr) {
    builder.add(VPackValue("outVariableNew"));
    _outVariableNew->toVelocyPack(builder);
  }
  builder.add(VPackValue("modificationFlags"));

  _options.toVelocyPack(builder);
}

/// @brief estimateCost
/// Note that all the modifying nodes use this estimateCost method which is
/// why we can make it final here.
CostEstimate ModificationNode::estimateCost() const {
  CostEstimate estimate = _dependencies.at(0)->getCost();
  estimate.estimatedCost += estimate.estimatedNrItems;
  if (_outVariableOld == nullptr && _outVariableNew == nullptr && !_producesResults) {
    // node produces no output
    estimate.estimatedNrItems = 0;
  }
  return estimate;
}

void ModificationNode::cloneCommon(ModificationNode* c) const {
  if (!_countStats) {
    c->disableStatistics();
  }
  c->producesResults(_producesResults);
  CollectionAccessingNode::cloneInto(*c);
}

///////////////////////////////////////////////////////////////////////////////
/// REMOVE
///
using AllRowsRemoveExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, RemoveModifier>>;
using SingleRowRemoveExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, RemoveModifier>>;

RemoveNode::RemoveNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")) {}

void RemoveNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  ModificationNode::toVelocyPackHelper(nodes, flags, seen);
  ModificationNode::toVelocyPackHelperPrimaryIndex(nodes);
  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);

  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> RemoveNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();

  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDocRegister = variableToRegisterId(_inVariable);
  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  OperationOptions options =
      ModificationExecutorHelpers::convertOptions(_options, _outVariableNew, _outVariableOld);

  auto readableInputRegisters = RegIdSet{inDocRegister};
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters), std::move(writableOutputRegisters));

  auto executorInfos = ModificationExecutorInfos(
      inDocRegister, RegisterPlan::MaxRegisterId, RegisterPlan::MaxRegisterId,
      outputNew, outputOld, RegisterPlan::MaxRegisterId /*output*/,
      _plan->getAst()->query(), std::move(options), collection(),
      ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(false) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));

  if (_options.readCompleteInput) {
    return std::make_unique<AllRowsRemoveExecutionBlock>(&engine, this,
                                                         std::move(registerInfos),
                                                         std::move(executorInfos));
  } else {
    return std::make_unique<SingleRowRemoveExecutionBlock>(&engine, this,
                                                           std::move(registerInfos),
                                                           std::move(executorInfos));
  }
}

/// @brief clone ExecutionNode recursively
ExecutionNode* RemoveNode::clone(ExecutionPlan* plan, bool withDependencies,
                                 bool withProperties) const {
  auto outVariableOld = _outVariableOld;
  auto inVariable = _inVariable;

  if (withProperties) {
    if (_outVariableOld != nullptr) {
      outVariableOld = plan->getAst()->variables()->createVariable(outVariableOld);
    }
    inVariable = plan->getAst()->variables()->createVariable(inVariable);
  }

  auto c = std::make_unique<RemoveNode>(plan, _id, collection(), _options,
                                        inVariable, outVariableOld);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

///////////////////////////////////////////////////////////////////////////////
/// INSERT
///
using AllRowsInsertExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, InsertModifier>>;
using SingleRowInsertExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, InsertModifier>>;

InsertNode::InsertNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inVariable(Variable::varFromVPack(plan->getAst(), base, "inVariable")) {}

/// @brief toVelocyPack
void InsertNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  ModificationNode::toVelocyPackHelper(nodes, flags, seen);  // call base class method

  // Now put info about vocbase and cid in there
  nodes.add(VPackValue("inVariable"));
  _inVariable->toVelocyPack(nodes);

  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> InsertNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  using namespace arangodb::aql;

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inputRegister = variableToRegisterId(_inVariable);

  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  OperationOptions options =
      ModificationExecutorHelpers::convertOptions(_options, _outVariableNew, _outVariableOld);

  auto readableInputRegisters = RegIdSet{inputRegister};
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters), std::move(writableOutputRegisters));

  ModificationExecutorInfos infos(
      inputRegister, RegisterPlan::MaxRegisterId, RegisterPlan::MaxRegisterId,
      outputNew, outputOld, RegisterPlan::MaxRegisterId /*output*/,
      _plan->getAst()->query(), std::move(options), collection(),
      ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(false) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));

  if (_options.readCompleteInput) {
    return std::make_unique<AllRowsInsertExecutionBlock>(&engine, this,
                                                         std::move(registerInfos),
                                                         std::move(infos));
  } else {
    return std::make_unique<SingleRowInsertExecutionBlock>(&engine, this,
                                                           std::move(registerInfos),
                                                           std::move(infos));
  }
}

/// @brief clone ExecutionNode recursively
ExecutionNode* InsertNode::clone(ExecutionPlan* plan, bool withDependencies,
                                 bool withProperties) const {
  auto outVariableOld = _outVariableOld;
  auto outVariableNew = _outVariableNew;
  auto inVariable = _inVariable;

  if (withProperties) {
    if (_outVariableNew != nullptr) {
      outVariableNew = plan->getAst()->variables()->createVariable(outVariableNew);
    }
    if (_outVariableOld != nullptr) {
      outVariableOld = plan->getAst()->variables()->createVariable(outVariableOld);
    }
    inVariable = plan->getAst()->variables()->createVariable(inVariable);
  }

  auto c = std::make_unique<InsertNode>(plan, _id, collection(), _options,
                                        inVariable, outVariableOld, outVariableNew);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

///////////////////////////////////////////////////////////////////////////////
/// REMOVE
///
using AllRowsUpdateReplaceExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, UpdateReplaceModifier>>;
using SingleRowUpdateReplaceExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpdateReplaceModifier>>;

UpdateReplaceNode::UpdateReplaceNode(ExecutionPlan* plan,
                                     arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inDocVariable(
          Variable::varFromVPack(plan->getAst(), base, "inDocVariable")),
      _inKeyVariable(
          Variable::varFromVPack(plan->getAst(), base, "inKeyVariable", true)) {}

void UpdateReplaceNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                           std::unordered_set<ExecutionNode const*>& seen) const {
  ModificationNode::toVelocyPackHelper(nodes, flags, seen);
  nodes.add(VPackValue("inDocVariable"));
  _inDocVariable->toVelocyPack(nodes);

  // inKeyVariable might be empty
  if (_inKeyVariable != nullptr) {
    nodes.add(VPackValue("inKeyVariable"));
    _inKeyVariable->toVelocyPack(nodes);
  }
}

UpdateNode::UpdateNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : UpdateReplaceNode(plan, base) {}

/// @brief toVelocyPack
void UpdateNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  UpdateReplaceNode::toVelocyPackHelper(nodes, flags, seen);
  ModificationNode::toVelocyPackHelperPrimaryIndex(nodes);
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> UpdateNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  using namespace arangodb::aql;

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDocRegister = variableToRegisterId(_inDocVariable);

  RegisterId inKeyRegister = variableToRegisterOptionalId(_inKeyVariable);
  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);
  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  auto readableInputRegisters = RegIdSet{inDocRegister};
  if (inKeyRegister < RegisterPlan::MaxRegisterId) {
    readableInputRegisters.emplace(inKeyRegister);
  }
  auto writableOutputRegisters = RegIdSet{};
  if (outputNew < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters), std::move(writableOutputRegisters));

  OperationOptions options =
      ModificationExecutorHelpers::convertOptions(_options, _outVariableNew, _outVariableOld);

  auto executorInfos = ModificationExecutorInfos(
      inDocRegister, inKeyRegister, RegisterPlan::MaxRegisterId, outputNew, outputOld,
      RegisterPlan::MaxRegisterId /*output*/, _plan->getAst()->query(),
      std::move(options), collection(), ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(false) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  if (_options.readCompleteInput) {
    return std::make_unique<AllRowsUpdateReplaceExecutionBlock>(&engine, this,
                                                                std::move(registerInfos),
                                                                std::move(executorInfos));
  } else {
    return std::make_unique<SingleRowUpdateReplaceExecutionBlock>(
        &engine, this, std::move(registerInfos), std::move(executorInfos));
  }
}

/// @brief clone ExecutionNode recursively
ExecutionNode* UpdateNode::clone(ExecutionPlan* plan, bool withDependencies,
                                 bool withProperties) const {
  auto outVariableOld = _outVariableOld;
  auto outVariableNew = _outVariableNew;
  auto inKeyVariable = _inKeyVariable;
  auto inDocVariable = _inDocVariable;

  if (withProperties) {
    if (_outVariableOld != nullptr) {
      outVariableOld = plan->getAst()->variables()->createVariable(outVariableOld);
    }
    if (_outVariableNew != nullptr) {
      outVariableNew = plan->getAst()->variables()->createVariable(outVariableNew);
    }
    if (inKeyVariable != nullptr) {
      inKeyVariable = plan->getAst()->variables()->createVariable(inKeyVariable);
    }
    inDocVariable = plan->getAst()->variables()->createVariable(inDocVariable);
  }

  auto c = std::make_unique<UpdateNode>(plan, _id, collection(), _options, inDocVariable,
                                        inKeyVariable, outVariableOld, outVariableNew);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

ReplaceNode::ReplaceNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : UpdateReplaceNode(plan, base) {}

/// @brief toVelocyPack
void ReplaceNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                     std::unordered_set<ExecutionNode const*>& seen) const {
  UpdateReplaceNode::toVelocyPackHelper(nodes, flags, seen);
  ModificationNode::toVelocyPackHelperPrimaryIndex(nodes);
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> ReplaceNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDocRegister = variableToRegisterId(_inDocVariable);

  RegisterId inKeyRegister = variableToRegisterOptionalId(_inKeyVariable);

  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);

  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  auto readableInputRegisters = RegIdSet{inDocRegister};
  if (inKeyRegister < RegisterPlan::MaxRegisterId) {
    readableInputRegisters.emplace(inKeyRegister);
  }
  auto writableOutputRegisters = RegIdSet{};;
  if (outputNew < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters), std::move(writableOutputRegisters));

  OperationOptions options =
      ModificationExecutorHelpers::convertOptions(_options, _outVariableNew, _outVariableOld);

  auto executorInfos = ModificationExecutorInfos(
      inDocRegister, inKeyRegister, RegisterPlan::MaxRegisterId, outputNew, outputOld,
      RegisterPlan::MaxRegisterId /*output*/, _plan->getAst()->query(),
      std::move(options), collection(), ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(true), IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));
  if (_options.readCompleteInput) {
    return std::make_unique<AllRowsUpdateReplaceExecutionBlock>(&engine, this,
                                                                std::move(registerInfos),
                                                                std::move(executorInfos));
  } else {
    return std::make_unique<SingleRowUpdateReplaceExecutionBlock>(
        &engine, this, std::move(registerInfos), std::move(executorInfos));
  }
}

/// @brief clone ExecutionNode recursively
ExecutionNode* ReplaceNode::clone(ExecutionPlan* plan, bool withDependencies,
                                  bool withProperties) const {
  auto outVariableOld = _outVariableOld;
  auto outVariableNew = _outVariableNew;
  auto inKeyVariable = _inKeyVariable;
  auto inDocVariable = _inDocVariable;

  if (withProperties) {
    if (_outVariableOld != nullptr) {
      outVariableOld = plan->getAst()->variables()->createVariable(outVariableOld);
    }
    if (_outVariableNew != nullptr) {
      outVariableNew = plan->getAst()->variables()->createVariable(outVariableNew);
    }
    if (inKeyVariable != nullptr) {
      inKeyVariable = plan->getAst()->variables()->createVariable(inKeyVariable);
    }
    inDocVariable = plan->getAst()->variables()->createVariable(inDocVariable);
  }

  auto c = std::make_unique<ReplaceNode>(plan, _id, collection(), _options, inDocVariable,
                                         inKeyVariable, outVariableOld, outVariableNew);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies, withProperties);
}

///////////////////////////////////////////////////////////////////////////////
/// UPSERT
///
using AllRowsUpsertExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<AllRowsFetcher, UpsertModifier>>;
using SingleRowUpsertExecutionBlock =
    ExecutionBlockImpl<ModificationExecutor<SingleRowFetcher<BlockPassthrough::Disable>, UpsertModifier>>;

UpsertNode::UpsertNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base)
    : ModificationNode(plan, base),
      _inDocVariable(
          Variable::varFromVPack(plan->getAst(), base, "inDocVariable")),
      _insertVariable(
          Variable::varFromVPack(plan->getAst(), base, "insertVariable")),
      _updateVariable(
          Variable::varFromVPack(plan->getAst(), base, "updateVariable")),
      _isReplace(base.get("isReplace").getBoolean()) {}

/// @brief toVelocyPack
void UpsertNode::toVelocyPackHelper(VPackBuilder& nodes, unsigned flags,
                                    std::unordered_set<ExecutionNode const*>& seen) const {
  ModificationNode::toVelocyPackHelper(nodes, flags, seen);
  ModificationNode::toVelocyPackHelperPrimaryIndex(nodes);

  nodes.add(VPackValue("inDocVariable"));
  _inDocVariable->toVelocyPack(nodes);
  nodes.add(VPackValue("insertVariable"));
  _insertVariable->toVelocyPack(nodes);
  nodes.add(VPackValue("updateVariable"));
  _updateVariable->toVelocyPack(nodes);
  nodes.add("isReplace", VPackValue(_isReplace));

  // And close it:
  nodes.close();
}

/// @brief creates corresponding ExecutionBlock
std::unique_ptr<ExecutionBlock> UpsertNode::createBlock(
    ExecutionEngine& engine, std::unordered_map<ExecutionNode*, ExecutionBlock*> const&) const {
  using namespace arangodb::aql;

  ExecutionNode const* previousNode = getFirstDependency();
  TRI_ASSERT(previousNode != nullptr);

  RegisterId inDoc = variableToRegisterId(_inDocVariable);
  RegisterId insert = variableToRegisterId(_insertVariable);
  RegisterId update = variableToRegisterId(_updateVariable);

  RegisterId outputNew = variableToRegisterOptionalId(_outVariableNew);

  RegisterId outputOld = variableToRegisterOptionalId(_outVariableOld);

  auto readableInputRegisters = RegIdSet{inDoc, insert, update};
  auto writableOutputRegisters = RegIdSet{};;
  if (outputNew < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputNew);
  }
  if (outputOld < RegisterPlan::MaxRegisterId) {
    writableOutputRegisters.emplace(outputOld);
  }
  auto registerInfos = createRegisterInfos(std::move(readableInputRegisters), std::move(writableOutputRegisters));

  OperationOptions options =
      ModificationExecutorHelpers::convertOptions(_options, _outVariableNew, _outVariableOld);

  auto executorInfos = ModificationExecutorInfos(
      inDoc, insert, update, outputNew, outputOld,
      RegisterPlan::MaxRegisterId /*output*/, _plan->getAst()->query(),
      std::move(options), collection(), ProducesResults(producesResults()),
      ConsultAqlWriteFilter(_options.consultAqlWriteFilter),
      IgnoreErrors(_options.ignoreErrors), DoCount(countStats()),
      IsReplace(_isReplace) /*(needed by upsert)*/,
      IgnoreDocumentNotFound(_options.ignoreDocumentNotFound));

    return std::make_unique<SingleRowUpsertExecutionBlock>(&engine, this,
                                                           std::move(registerInfos),
                                                           std::move(executorInfos));

}

/// @brief clone ExecutionNode recursively
ExecutionNode* UpsertNode::clone(ExecutionPlan* plan, bool withDependencies,
                                 bool withProperties) const {
  auto outVariableNew = _outVariableNew;
  auto inDocVariable = _inDocVariable;
  auto insertVariable = _insertVariable;
  auto updateVariable = _updateVariable;

  if (withProperties) {
    if (_outVariableNew != nullptr) {
      outVariableNew = plan->getAst()->variables()->createVariable(outVariableNew);
    }
    inDocVariable = plan->getAst()->variables()->createVariable(inDocVariable);
    insertVariable = plan->getAst()->variables()->createVariable(insertVariable);
    updateVariable = plan->getAst()->variables()->createVariable(updateVariable);
  }

  auto c = std::make_unique<UpsertNode>(plan, _id, collection(), _options,
                                        inDocVariable, insertVariable,
                                        updateVariable, outVariableNew, _isReplace);
  ModificationNode::cloneCommon(c.get());

  return cloneHelper(std::move(c), withDependencies, withProperties);
}
}  // namespace aql
}  // namespace arangodb
