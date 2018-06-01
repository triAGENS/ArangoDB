////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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

#include "BasicBlocks.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Basics/Exceptions.h"
#include "VocBase/vocbase.h"

using namespace arangodb::aql;

SingletonBlock::SingletonBlock(ExecutionEngine* engine, SingletonNode const* ep)
    : ExecutionBlock(engine, ep) {
  auto en = ExecutionNode::castTo<SingletonNode const*>(getPlanNode());
  auto const& registerPlan = en->getRegisterPlan()->varInfo;
  std::unordered_set<Variable const*> const& varsUsedLater = en->getVarsUsedLater();

  for (auto const& it : varsUsedLater) {
    auto it2 = registerPlan.find(it->id);

    if (it2 != registerPlan.end()) {
      _whitelist.emplace((*it2).second.registerId);
    }
  }
}
  
/// @brief initializeCursor, store a copy of the register values coming from
/// above
std::pair<ExecutionState, arangodb::Result> SingletonBlock::initializeCursor(
    AqlItemBlock* items, size_t pos) {
  DEBUG_BEGIN_BLOCK();
  // Create a deep copy of the register values given to us:
  if (items != nullptr) {
    // build a whitelist with all the registers that we will copy from above
    _inputRegisterValues.reset(items->slice(pos, _whitelist));
  }

  _done = false;
  return {ExecutionState::DONE, TRI_ERROR_NO_ERROR};

  // cppcheck-suppress style
  DEBUG_END_BLOCK();  
}

/// @brief shutdown the singleton block
int SingletonBlock::shutdown(int errorCode) {
  _inputRegisterValues.reset();
  return ExecutionBlock::shutdown(errorCode);
}

int SingletonBlock::getOrSkipSomeOld(size_t atMost, bool skipping,
                                  AqlItemBlock*& result, size_t& skipped) {
  DEBUG_BEGIN_BLOCK();  
  TRI_ASSERT(result == nullptr && skipped == 0);

  if (_done) {
    return TRI_ERROR_NO_ERROR;
  }

  if (!skipping) {
    result = requestBlock(1, getPlanNode()->getRegisterPlan()->nrRegs[getPlanNode()->getDepth()]);

    try {
      if (_inputRegisterValues != nullptr) {
        skipped++;
        for (RegisterId reg = 0; reg < _inputRegisterValues->getNrRegs();
             ++reg) {
          if (_whitelist.find(reg) == _whitelist.end()) {
            continue;
          }
          TRI_IF_FAILURE("SingletonBlock::getOrSkipSome") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }

          AqlValue a = _inputRegisterValues->getValue(0, reg);
          if (a.isEmpty()) {
            continue;
          }
          _inputRegisterValues->steal(a);

          try {
            TRI_IF_FAILURE("SingletonBlock::getOrSkipSomeSet") {
              THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
            }

            result->setValue(0, reg, a);
          } catch (...) {
            a.destroy();
            throw;
          }
          _inputRegisterValues->eraseValue(0, reg);
        }
      }
    } catch (...) {
      delete result;
      result = nullptr;
      throw;
    }
  } else {
    if (_inputRegisterValues != nullptr) {
      skipped++;
    }
  }

  _done = true;
  return TRI_ERROR_NO_ERROR;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();  
}

FilterBlock::FilterBlock(ExecutionEngine* engine, FilterNode const* en)
    : ExecutionBlock(engine, en), 
      _inReg(ExecutionNode::MaxRegisterId),
      _collector(&engine->_itemBlockManager) {

  auto it = en->getRegisterPlan()->varInfo.find(en->_inVariable->id);
  TRI_ASSERT(it != en->getRegisterPlan()->varInfo.end());
  _inReg = it->second.registerId;
  TRI_ASSERT(_inReg < ExecutionNode::MaxRegisterId);
}

FilterBlock::~FilterBlock() {}
  
/// @brief internal function to actually decide if the document should be used
bool FilterBlock::takeItem(AqlItemBlock* items, size_t index) const {
  return items->getValueReference(index, _inReg).toBoolean();
}

/// @brief internal function to get another block
bool FilterBlock::getBlock(size_t atMost) {
  DEBUG_BEGIN_BLOCK();  
  while (true) {  // will be left by break or return
    if (!ExecutionBlock::getBlock(atMost)) {
      return false;
    }

    if (_buffer.size() > 1) {
      break;  // Already have a current block
    }

    // Now decide about these docs:
    AqlItemBlock* cur = _buffer.front();

    _chosen.clear();
    _chosen.reserve(cur->size());
    for (size_t i = 0; i < cur->size(); ++i) {
      if (takeItem(cur, i)) {
        TRI_IF_FAILURE("FilterBlock::getBlock") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        _chosen.emplace_back(i);
      }
    }

    _engine->_stats.filtered += (cur->size() - _chosen.size());

    if (!_chosen.empty()) {
      break;  // OK, there are some docs in the result
    }

    _buffer.pop_front();  // Block was useless, just try again
    returnBlock(cur);     // free this block
  
    throwIfKilled();  // check if we were aborted
  }

  return true;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();  
}

int FilterBlock::getOrSkipSomeOld(size_t atMost, bool skipping,
                               AqlItemBlock*& result, size_t& skipped) {
  DEBUG_BEGIN_BLOCK();  
  TRI_ASSERT(result == nullptr && skipped == 0);

  if (_done) {
    return TRI_ERROR_NO_ERROR;
  }

  // if _buffer.size() is > 0 then _pos is valid
  _collector.clear();

  while (skipped < atMost) {
    if (_buffer.empty()) {
      if (!getBlock(atMost - skipped)) {
        _done = true;
        break;
      }
      _pos = 0;
    }

    // If we get here, then _buffer.size() > 0 and _pos points to a
    // valid place in it.
    AqlItemBlock* cur = _buffer.front();
    if (_chosen.size() - _pos + skipped > atMost) {
      // The current block of chosen ones is too large for atMost:
      if (!skipping) {
        std::unique_ptr<AqlItemBlock> more(
            cur->slice(_chosen, _pos, _pos + (atMost - skipped)));

        TRI_IF_FAILURE("FilterBlock::getOrSkipSome1") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        _collector.add(std::move(more));
      }
      _pos += atMost - skipped;
      skipped = atMost;
    } else if (_pos > 0 || _chosen.size() < cur->size()) {
      // The current block fits into our result, but it is already
      // half-eaten or needs to be copied anyway:
      if (!skipping) {
        std::unique_ptr<AqlItemBlock> more(
            cur->steal(_chosen, _pos, _chosen.size()));

        TRI_IF_FAILURE("FilterBlock::getOrSkipSome2") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }

        _collector.add(std::move(more));
      }
      skipped += _chosen.size() - _pos;
      returnBlock(cur);
      _buffer.pop_front();
      _chosen.clear();
      _pos = 0;
    } else {
      // The current block fits into our result and is fresh and
      // takes them all, so we can just hand it on:
      skipped += cur->size();
      if (!skipping) {
        // if any of the following statements throw, then cur is not lost,
        // as it is still contained in _buffer
        TRI_IF_FAILURE("FilterBlock::getOrSkipSome3") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
        _collector.add(cur);
      } else {
        returnBlock(cur);
      }
      _buffer.pop_front();
      _chosen.clear();
      _pos = 0;
    }
  }

  if (!skipping) {
    result = _collector.steal();
  }
  return TRI_ERROR_NO_ERROR;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();  
}

std::pair<ExecutionState, arangodb::Result> LimitBlock::initializeCursor(
    AqlItemBlock* items, size_t pos) {
  DEBUG_BEGIN_BLOCK();  
  auto res = ExecutionBlock::initializeCursor(items, pos);
  if (res.first == ExecutionState::WAITING ||
      !res.second.ok()) {
    // If we need to wait or get an error we return as is.
    return res;
  }

  _state = 0;
  _count = 0;
  return res;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();  
}

int LimitBlock::getOrSkipSomeOld(size_t atMost, bool skipping,
                              AqlItemBlock*& result, size_t& skipped) {
  DEBUG_BEGIN_BLOCK();
  TRI_ASSERT(result == nullptr && skipped == 0);

  if (_state == 2) {
    return TRI_ERROR_NO_ERROR;
  }

  if (_state == 0) {
    if (_fullCount) {
      _engine->_stats.fullCount = 0;
    }

    if (_offset > 0) {
      size_t numActuallySkipped = 0;
      _dependencies[0]->skip(_offset, numActuallySkipped);
      if (_fullCount) {
        _engine->_stats.fullCount += static_cast<int64_t>(numActuallySkipped);
      }
    }
    _state = 1;
    _count = 0;
    if (_limit == 0 && !_fullCount) {
      // quick exit for limit == 0
      _state = 2;
      return TRI_ERROR_NO_ERROR;
    }
  }

  // If we get to here, _state == 1 and _count < _limit
  if (_limit > 0) {
    if (atMost > _limit - _count) {
      atMost = _limit - _count;
    }

    ExecutionBlock::getOrSkipSomeOld(atMost, skipping, result, skipped);

    if (skipped == 0) {
      return TRI_ERROR_NO_ERROR;
    }

    _count += skipped;
    if (_fullCount) {
      _engine->_stats.fullCount += static_cast<int64_t>(skipped);
    }
  }

  if (_count >= _limit) {
    _state = 2;

    if (_fullCount) {
      // if fullCount is set, we must fetch all elements from the
      // dependency. we'll use the default batch size for this
      atMost = DefaultBatchSize();

      // suck out all data from the dependencies
      while (true) {
        skipped = 0;
        AqlItemBlock* ignore = nullptr;
        ExecutionBlock::getOrSkipSomeOld(atMost, skipping, ignore, skipped);

        TRI_ASSERT(ignore == nullptr || ignore->size() == skipped);
        _engine->_stats.fullCount += skipped;
        delete ignore;

        if (skipped == 0) {
          break;
        }
      }
    }
  }

  return TRI_ERROR_NO_ERROR;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

AqlItemBlock* ReturnBlock::getSomeOld(size_t atMost) {
  DEBUG_BEGIN_BLOCK();
  traceGetSomeBegin(atMost);
  
  auto ep = ExecutionNode::castTo<ReturnNode const*>(getPlanNode());

  std::unique_ptr<AqlItemBlock> res(
      ExecutionBlock::getSomeWithoutRegisterClearoutOld(atMost));

  if (res == nullptr) {
    traceGetSomeEnd(nullptr);
    return nullptr;
  }

  if (_returnInheritedResults) {
    if (ep->_count) {
    _engine->_stats.count += static_cast<int64_t>(res->size());
    }
    traceGetSomeEnd(res.get());
    return res.release();
  }

  size_t const n = res->size();

  // Let's steal the actual result and throw away the vars:
  auto it = ep->getRegisterPlan()->varInfo.find(ep->_inVariable->id);
  TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
  RegisterId const registerId = it->second.registerId;

  std::unique_ptr<AqlItemBlock> stripped(requestBlock(n, 1));

  for (size_t i = 0; i < n; i++) {
    auto a = res->getValueReference(i, registerId);

    if (!a.isEmpty()) {
      if (a.requiresDestruction()) {
        res->steal(a);

        try {
          TRI_IF_FAILURE("ReturnBlock::getSome") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }

          stripped->setValue(i, 0, a);
        } catch (...) {
          a.destroy();
          throw;
        }
        // If the following does not go well, we do not care, since
        // the value is already stolen and installed in stripped
        res->eraseValue(i, registerId);
      } else {
        stripped->setValue(i, 0, a);
      }
    }
  }
        
  if (ep->_count) {
    _engine->_stats.count += static_cast<int64_t>(n);
  }

  traceGetSomeEnd(stripped.get());
  return stripped.release();

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

/// @brief make the return block return the results inherited from above,
/// without creating new blocks
/// returns the id of the register the final result can be found in
RegisterId ReturnBlock::returnInheritedResults() {
  DEBUG_BEGIN_BLOCK();
  _returnInheritedResults = true;

  auto ep = ExecutionNode::castTo<ReturnNode const*>(getPlanNode());
  auto it = ep->getRegisterPlan()->varInfo.find(ep->_inVariable->id);
  TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());

  return it->second.registerId;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

/// @brief initializeCursor, only call base
std::pair<ExecutionState, arangodb::Result> NoResultsBlock::initializeCursor(
    AqlItemBlock* items, size_t pos) {
  DEBUG_BEGIN_BLOCK();  
  _done = true;
  return {ExecutionState::DONE, TRI_ERROR_NO_ERROR};

  // cppcheck-suppress style
  DEBUG_END_BLOCK();  
}

int NoResultsBlock::getOrSkipSomeOld(size_t,  // atMost
                                  bool,    // skipping
                                  AqlItemBlock*& result, size_t& skipped) {
  TRI_ASSERT(result == nullptr && skipped == 0);
  return TRI_ERROR_NO_ERROR;
}
