////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "ExecutionBlockMock.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                 ExecutionNodeMock
// -----------------------------------------------------------------------------

ExecutionNodeMock::ExecutionNodeMock(size_t id /*= 0*/)
  : ExecutionNode(nullptr, id) {
  setVarUsageValid();
  planRegisters();
}

arangodb::aql::ExecutionNode::NodeType ExecutionNodeMock::getType() const {
  return arangodb::aql::ExecutionNode::NodeType::SINGLETON;
}
  
std::unique_ptr<arangodb::aql::ExecutionBlock> ExecutionNodeMock::createBlock(
    arangodb::aql::ExecutionEngine& engine,
    std::unordered_map<ExecutionNode*, arangodb::aql::ExecutionBlock*> const& cache
) const {
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "cannot create a block of ExecutionNodeMock");
}

arangodb::aql::ExecutionNode* ExecutionNodeMock::clone(
    arangodb::aql::ExecutionPlan* plan,
    bool withDependencies,
    bool withProperties
) const {
  return new ExecutionNodeMock(id());
}

void ExecutionNodeMock::toVelocyPackHelper(
    arangodb::velocypack::Builder& nodes,
    unsigned flags
) const {
  ExecutionNode::toVelocyPackHelperGeneric(nodes, flags);  // call base class method
  nodes.close();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                ExecutionBlockMock
// -----------------------------------------------------------------------------

ExecutionBlockMock::ExecutionBlockMock(
    arangodb::aql::AqlItemBlock const& data,
    arangodb::aql::ExecutionEngine& engine,
    arangodb::aql::ExecutionNode const& node
) : arangodb::aql::ExecutionBlock(&engine, &node),
    _data(&data) {
}

int ExecutionBlockMock::initializeCursor(
    arangodb::aql::AqlItemBlock* items, size_t pos
) {
  DEBUG_BEGIN_BLOCK();
  const int res = ExecutionBlock::initializeCursor(items, pos);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  _pos_in_data = 0;
  DEBUG_END_BLOCK();

  return TRI_ERROR_NO_ERROR;
}

arangodb::aql::AqlItemBlock* ExecutionBlockMock::getSome(
    size_t atMost
) {
  DEBUG_BEGIN_BLOCK();
  traceGetSomeBegin(atMost);

  if (_done) {
    traceGetSomeEnd(nullptr);
    return nullptr;
  }

  bool needMore;
  arangodb::aql::AqlItemBlock* cur = nullptr;
  std::unique_ptr<arangodb::aql::AqlItemBlock> res;

  do {
    needMore = false;

    if (_buffer.empty()) {
      size_t const toFetch = (std::min)(DefaultBatchSize(), atMost);
      if (!ExecutionBlock::getBlock(toFetch)) {
        _done = true;
        return nullptr;
      }
      _pos = 0;  // this is in the first block
    }

    TRI_ASSERT(!_buffer.empty());
    cur = _buffer.front();

    if (_pos_in_data == _data->size()) {
      needMore = true;
      _pos_in_data = 0;

      if (++_pos >= cur->size()) {
        _buffer.pop_front();  // does not throw
        returnBlock(cur);
        _pos = 0;
      }
    }
  } while (needMore);

  TRI_ASSERT(cur);

  auto const from = std::min(_pos_in_data, _data->size());
  auto const to = std::min(_pos_in_data + atMost, _data->size());
  res.reset(_data->slice(from, to));

  // only copy 1st row of registers inherited from previous frame(s)
  inheritRegisters(cur, res.get(), _pos);

  throwIfKilled(); // check if we were aborted

  TRI_IF_FAILURE("ExecutionBlockMock::moreDocuments") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  _pos_in_data = to;
  TRI_ASSERT(res);

  if (res->size() < atMost) {
    // The collection did not have enough results
    res->shrink(res->size());
  }

  // Clear out registers no longer needed later:
  clearRegisters(res.get());

  traceGetSomeEnd(res.get());

  return res.release();

  DEBUG_END_BLOCK();
}

size_t ExecutionBlockMock::skipSome(size_t atMost) {
  DEBUG_BEGIN_BLOCK();

  if (_done) {
    return 0;
  }

  size_t skipped = 0;

  while (skipped < atMost) {
    if (_buffer.empty()) {
      size_t toFetch = (std::min)(DefaultBatchSize(), atMost);
      if (!getBlock(toFetch)) {
        _done = true;
        return skipped;
      }
      _pos = 0;  // this is in the first block
      _pos_in_data = 0;
    }

    TRI_ASSERT(!_buffer.empty());
    arangodb::aql::AqlItemBlock* cur = _buffer.front();

    TRI_ASSERT(_data->size() >= _pos_in_data);
    skipped += std::min(_data->size() - _pos_in_data, atMost - skipped);
    _pos_in_data += skipped;

    if (skipped < atMost) {
      // not skipped enough re-initialize fetching of documents
      if (++_pos >= cur->size()) {
        _buffer.pop_front();  // does not throw
        returnBlock(cur);
        _pos = 0;
      } else {
        // we have exhausted this cursor
        // re-initialize fetching of documents
        _pos_in_data = 0;
      }
    }
  }

  // We skipped atLeast documents
  return skipped;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}
