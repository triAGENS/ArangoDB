////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "DistributeExecutor.h"

#include "Aql/AqlCallStack.h"
#include "Aql/ClusterNodes.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/IdExecutor.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/Query.h"
#include "Aql/RegisterPlan.h"
#include "Aql/ShadowAqlItemRow.h"
#include "Aql/SkipResult.h"
#include "Basics/StaticStrings.h"
#include "Transaction/Helpers.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Collection.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

DistributeExecutorInfos::DistributeExecutorInfos(
    std::vector<std::string> clientIds, Collection const* collection,
    RegisterId regId, RegisterId alternativeRegId, bool allowSpecifiedKeys,
    bool allowKeyConversionToObject, bool createKeys, bool fixupGraphInput,
     ScatterNode::ScatterType type)
    : ClientsExecutorInfos(std::move(clientIds)),
      _regId(regId),
      _alternativeRegId(alternativeRegId),
      _allowKeyConversionToObject(allowKeyConversionToObject),
      _createKeys(createKeys),
      _usesDefaultSharding(collection->usesDefaultSharding()),
      _allowSpecifiedKeys(allowSpecifiedKeys),
      _fixupGraphInput(fixupGraphInput),
      _collection(collection),
      _logCol(collection->getCollection()),
      _type(type) {}

auto DistributeExecutorInfos::registerId() const noexcept -> RegisterId {
  TRI_ASSERT(_regId != RegisterPlan::MaxRegisterId);
  return _regId;
}
auto DistributeExecutorInfos::hasAlternativeRegister() const noexcept -> bool {
  return _alternativeRegId != RegisterPlan::MaxRegisterId;
}
auto DistributeExecutorInfos::alternativeRegisterId() const noexcept -> RegisterId {
  TRI_ASSERT(_alternativeRegId != RegisterPlan::MaxRegisterId);
  return _alternativeRegId;
}

auto DistributeExecutorInfos::allowKeyConversionToObject() const noexcept -> bool {
  return _allowKeyConversionToObject;
}

auto DistributeExecutorInfos::createKeys() const noexcept -> bool {
  return _createKeys;
}
auto DistributeExecutorInfos::usesDefaultSharding() const noexcept -> bool {
  return _usesDefaultSharding;
}
auto DistributeExecutorInfos::allowSpecifiedKeys() const noexcept -> bool {
  return _allowSpecifiedKeys;
}

auto DistributeExecutorInfos::needsToFixGraphInput() const -> bool {
  return _fixupGraphInput;
}

auto DistributeExecutorInfos::scatterType() const noexcept -> ScatterNode::ScatterType {
  return _type;
}

auto DistributeExecutorInfos::getResponsibleClient(arangodb::velocypack::Slice value) const
    -> ResultT<std::string> {
  std::string shardId;
  int res = _logCol->getResponsibleShard(value, true, shardId);

  if (res != TRI_ERROR_NO_ERROR) {
    return Result{res};
  }

  TRI_ASSERT(!shardId.empty());
  if (_type == ScatterNode::ScatterType::SERVER) {
    // Special case for server based distribution.
    shardId = _collection->getServerForShard(shardId);
    TRI_ASSERT(!shardId.empty());
  }
  return shardId;
}

/// @brief create a new document key
auto DistributeExecutorInfos::createKey(VPackSlice input) const -> std::string {
  return _logCol->createKey(input);
}

DistributeExecutor::ClientBlockData::ClientBlockData(ExecutionEngine& engine,
                                                     ScatterNode const* node,
                                                     RegisterInfos const& registerInfos)
    : _blockManager(engine.itemBlockManager()), registerInfos(registerInfos) {
  // We only get shared ptrs to const data. so we need to copy here...
  auto executorInfos = IdExecutorInfos{false, 0, "", false};
  auto idExecutorRegisterInfos =
      RegisterInfos{{},
                    {},
                    registerInfos.numberOfInputRegisters(),
                    registerInfos.numberOfOutputRegisters(),
                    *registerInfos.registersToClear(),
                    *registerInfos.registersToKeep()};
  // NOTE: Do never change this type! The execute logic below requires this and only this type.
  _executor = std::make_unique<ExecutionBlockImpl<IdExecutor<ConstFetcher>>>(
      &engine, node, std::move(idExecutorRegisterInfos), std::move(executorInfos));
}

auto DistributeExecutor::ClientBlockData::clear() -> void {
  _queue.clear();
  _executorHasMore = false;
}

auto DistributeExecutor::ClientBlockData::addBlock(SharedAqlItemBlockPtr block,
                                                   std::vector<size_t> usedIndexes) -> void {
  _queue.emplace_back(block, std::move(usedIndexes));
}

auto DistributeExecutor::ClientBlockData::addSkipResult(SkipResult const& skipResult) -> void {
  TRI_ASSERT(_skipped.subqueryDepth() == 1 ||
             _skipped.subqueryDepth() == skipResult.subqueryDepth());
  _skipped.merge(skipResult, false);
}

auto DistributeExecutor::ClientBlockData::hasDataFor(AqlCall const& call) -> bool {
  return _executorHasMore || !_queue.empty();
}

/**
 * @brief This call will join as many blocks as available from the queue
 *        and return them in a SingleBlock. We then use the IdExecutor
 *        to hand out the data contained in these blocks
 *        We do on purpose not give any kind of guarantees on the sizing of
 *        this block to be flexible with the implementation, and find a good
 *        trade-off between blocksize and block copy operations.
 *
 * @return SharedAqlItemBlockPtr a joind block from the queue.
 */
auto DistributeExecutor::ClientBlockData::popJoinedBlock()
    -> std::tuple<SharedAqlItemBlockPtr, SkipResult> {
  // There are some optimizations available in this implementation.
  // Namely we could apply good logic to cut the blocks at shadow rows
  // in order to allow the IDexecutor to hand them out en-block.
  // However we might leverage the restriction to stop at ShadowRows
  // at one point anyways, and this Executor has no business with ShadowRows.
  size_t numRows = 0;
  for (auto const& [block, choosen] : _queue) {
    numRows += choosen.size();
    if (numRows >= ExecutionBlock::DefaultBatchSize) {
      // Avoid to put too many rows into this block.
      break;
    }
  }

  SharedAqlItemBlockPtr newBlock =
      _blockManager.requestBlock(numRows, registerInfos.numberOfOutputRegisters());
  // We create a block, with correct register information
  // but we do not allow outputs to be written.
  OutputAqlItemRow output{newBlock, make_shared_unordered_set(),
                          registerInfos.registersToKeep(),
                          registerInfos.registersToClear()};
  while (!output.isFull()) {
    // If the queue is empty our sizing above would not be correct
    TRI_ASSERT(!_queue.empty());
    auto const& [block, choosen] = _queue.front();
    TRI_ASSERT(output.numRowsLeft() >= choosen.size());
    for (auto const& i : choosen) {
      // We do not really care what we copy. However
      // the API requires to know what it is.
      if (block->isShadowRow(i)) {
        ShadowAqlItemRow toCopy{block, i};
        output.moveRow(toCopy);
      } else {
        InputAqlItemRow toCopy{block, i};
        output.copyRow(toCopy);
      }
      output.advanceRow();
    }
    // All required rows copied.
    // Drop block form queue.
    _queue.pop_front();
  }
  SkipResult skip = _skipped;
  _skipped.reset();
  return {newBlock, skip};
}

auto DistributeExecutor::ClientBlockData::execute(AqlCallStack callStack, ExecutionState upstreamState)
    -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> {
  TRI_ASSERT(_executor != nullptr);
  // Make sure we actually have data before you call execute
  TRI_ASSERT(hasDataFor(callStack.peek()));
  if (!_executorHasMore) {
    // This cast is guaranteed, we create this a couple lines above and only
    // this executor is used here.
    // Unfortunately i did not get a version compiled were i could only forward
    // declare the templates in header.
    auto casted =
        static_cast<ExecutionBlockImpl<IdExecutor<ConstFetcher>>*>(_executor.get());
    TRI_ASSERT(casted != nullptr);
    auto [block, skipped] = popJoinedBlock();
    // We will at least get one block, otherwise the hasDataFor would
    // be required to return false!
    TRI_ASSERT(block != nullptr);

    casted->injectConstantBlock(block, skipped);
    _executorHasMore = true;
  }
  auto [state, skipped, result] = _executor->execute(callStack);

  // We have all data locally cannot wait here.
  TRI_ASSERT(state != ExecutionState::WAITING);

  if (state == ExecutionState::DONE) {
    // This executor is finished, including shadowrows
    // We are going to reset it on next call
    _executorHasMore = false;

    // Also we need to adjust the return states
    // as this state only represents one single block
    if (!_queue.empty()) {
      state = ExecutionState::HASMORE;
    } else {
      state = upstreamState;
    }
  }
  return {state, skipped, result};
}

DistributeExecutor::DistributeExecutor(DistributeExecutorInfos const& infos)
    : _infos(infos){};

auto DistributeExecutor::distributeBlock(SharedAqlItemBlockPtr block, SkipResult skipped,
                                         std::unordered_map<std::string, ClientBlockData>& blockMap)
    -> void {
  std::unordered_map<std::string, std::vector<std::size_t>> choosenMap;
  choosenMap.reserve(blockMap.size());
  for (size_t i = 0; i < block->size(); ++i) {
    if (block->isShadowRow(i)) {
      // ShadowRows need to be added to all Clients
      for (auto const& [key, value] : blockMap) {
        choosenMap[key].emplace_back(i);
      }
    } else {
      auto client = getClient(block, i);
      // We can only have clients we are prepared for
      TRI_ASSERT(blockMap.find(client) != blockMap.end());
      choosenMap[client].emplace_back(i);
    }
  }
  // We cannot have more in choosen than we have blocks
  TRI_ASSERT(choosenMap.size() <= blockMap.size());
  for (auto const& [key, value] : choosenMap) {
    TRI_ASSERT(blockMap.find(key) != blockMap.end());
    auto target = blockMap.find(key);
    if (target == blockMap.end()) {
      // Impossible, just avoid UB.
      LOG_TOPIC("7bae6", ERR, Logger::AQL)
          << "Tried to distribute data to shard " << key
          << " which is not part of the query. Ignoring.";
      continue;
    }
    target->second.addBlock(block, std::move(value));
  }

  // Add the skipResult to all clients.
  // It needs to be fetched once for every client.
  for (auto& [key, map] : blockMap) {
    map.addSkipResult(skipped);
  }
}

auto DistributeExecutor::getClientByIdSlice(VPackSlice input) -> std::string {
  // Need to fix this document.
  // We need id and key as input.
  auto keyPart = transaction::helpers::extractKeyPart(input);
  _keyBuilder.clear();
  _keyBuilder.openObject(true);
  _keyBuilder.add(StaticStrings::KeyString,
                  VPackValuePair(keyPart.data(), keyPart.size(), VPackValueType::String));
  _keyBuilder.close();
  // If _key is invalid, we will throw here.
  auto res = _infos.getResponsibleClient(_keyBuilder.slice());
  THROW_ARANGO_EXCEPTION_IF_FAIL(res.result());
  return res.get();
}

auto DistributeExecutor::getClient(SharedAqlItemBlockPtr block, size_t rowIndex)
    -> std::string {
  InputAqlItemRow row{block, rowIndex};
  AqlValue val = row.getValue(_infos.registerId());

  VPackSlice input = val.slice();  // will throw when wrong type

  bool usedAlternativeRegId = false;

  if (input.isNull() && _infos.hasAlternativeRegister()) {
    // value is set, but null
    // check if there is a second input register available (UPSERT makes use of
    // two input registers,
    // one for the search document, the other for the insert document)
    val = row.getValue(_infos.alternativeRegisterId());

    input = val.slice();  // will throw when wrong type
    usedAlternativeRegId = true;
  }

  VPackSlice value = input;
  if (_infos.needsToFixGraphInput()) {
    if (input.isString()) {
      return getClientByIdSlice(input);
    } else if (input.isObject() && input.hasKey(StaticStrings::IdString) && !input.hasKey(StaticStrings::KeyString)) {
      // The input is an object, but only contains an _id, not a _key value that could be extracted.
      // We can work with _id value only however so let us do this.
      return getClientByIdSlice(input.get(StaticStrings::IdString));
    }
    if (!input.isObject() || !input.hasKey(StaticStrings::IdString)) {
      // non objects cannot be sharded.
      // Need to throw here.
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     "invalid start vertex. Must either be "
                                     "an _id string or an object with _id."
                                     "Instead got: " + input.toJson());
    }
    // If the value is valid (like a document) it will simply work.
    auto res = _infos.getResponsibleClient(value);
    THROW_ARANGO_EXCEPTION_IF_FAIL(res.result());
    return res.get();
  } else {
    bool hasCreatedKeyAttribute = false;

    if (input.isString() && _infos.allowKeyConversionToObject()) {
      _keyBuilder.clear();
      _keyBuilder.openObject(true);
      _keyBuilder.add(StaticStrings::KeyString, input);
      _keyBuilder.close();

      // clear the previous value
      block->destroyValue(rowIndex, _infos.registerId());

      // overwrite with new value
      block->emplaceValue(rowIndex, _infos.registerId(), _keyBuilder.slice());

      value = _keyBuilder.slice();
      hasCreatedKeyAttribute = true;
    } else if (!input.isObject()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID);
    }

    TRI_ASSERT(value.isObject());

    if (_infos.createKeys()) {
      bool buildNewObject = false;
      // we are responsible for creating keys if none present

      if (_infos.usesDefaultSharding()) {
        // the collection is sharded by _key...
        if (!hasCreatedKeyAttribute && !value.hasKey(StaticStrings::KeyString)) {
          // there is no _key attribute present, so we are responsible for
          // creating one
          buildNewObject = true;
        }
      } else {
        // the collection is not sharded by _key
        if (hasCreatedKeyAttribute || value.hasKey(StaticStrings::KeyString)) {
          // a _key was given, but user is not allowed to specify _key
          if (usedAlternativeRegId || !_infos.allowSpecifiedKeys()) {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_MUST_NOT_SPECIFY_KEY);
          }
        } else {
          buildNewObject = true;
        }
      }

      if (buildNewObject) {
        _keyBuilder.clear();
        _keyBuilder.openObject(true);
        _keyBuilder.add(StaticStrings::KeyString, VPackValue(_infos.createKey(value)));
        _keyBuilder.close();

        _objectBuilder.clear();
        VPackCollection::merge(_objectBuilder, input, _keyBuilder.slice(), true);

        // clear the previous value and overwrite with new value:
        auto reg = usedAlternativeRegId ? _infos.alternativeRegisterId()
                                        : _infos.registerId();

        block->destroyValue(rowIndex, reg);
        block->emplaceValue(rowIndex, reg, _objectBuilder.slice());
        value = _objectBuilder.slice();
      }
    }
  }

  auto res = _infos.getResponsibleClient(value);
  THROW_ARANGO_EXCEPTION_IF_FAIL(res.result());
  return res.get();
}

ExecutionBlockImpl<DistributeExecutor>::ExecutionBlockImpl(ExecutionEngine* engine,
                                                           DistributeNode const* node,
                                                           RegisterInfos registerInfos,
                                                           DistributeExecutorInfos&& executorInfos)
    : BlocksWithClientsImpl(engine, node, std::move(registerInfos),
                            std::move(executorInfos)) {}
