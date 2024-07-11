////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "QueryCursor.h"

#include "Aql/AqlCall.h"
#include "Aql/AqlCallList.h"
#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Aql/SharedQueryState.h"
#include "Basics/ScopeGuard.h"
#include "Logger/LogMacros.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "VocBase/vocbase.h"

#include <absl/strings/str_cat.h>

#include <velocypack/Builder.h>
#include <velocypack/Dumper.h>
#include <velocypack/Options.h>

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::basics;

QueryResultCursor::QueryResultCursor(TRI_vocbase_t& vocbase,
                                     aql::QueryResult&& result,
                                     size_t batchSize, double ttl,
                                     bool hasCount, bool isRetriable)
    : Cursor(TRI_NewServerSpecificTick(), batchSize, ttl, hasCount,
             isRetriable),
      _guard(vocbase),
      _result(std::move(result)),
      _iterator(_result.data->slice()),
      _memoryUsageAtStart(_result.memoryUsage()),
      _cached(_result.cached) {
  TRI_ASSERT(_result.data->slice().isArray());
}

VPackSlice QueryResultCursor::extra() const {
  if (_result.extra) {
    _result.extra->slice();
  }
  return {};
}

/// @brief check whether the cursor contains more data
bool QueryResultCursor::hasNext() const noexcept { return _iterator.valid(); }

/// @brief return the next element
VPackSlice QueryResultCursor::next() {
  TRI_ASSERT(_result.data != nullptr);
  TRI_ASSERT(_iterator.valid());
  VPackSlice slice = _iterator.value();
  _iterator.next();
  return slice;
}

uint64_t QueryResultCursor::memoryUsage() const noexcept {
  return _memoryUsageAtStart;
}

/// @brief return the cursor size
size_t QueryResultCursor::count() const { return _iterator.size(); }

std::pair<ExecutionState, Result> QueryResultCursor::dump(
    VPackBuilder& builder) {
  // This cursor cannot block, result already there.
  auto res = dumpSync(builder);
  return {ExecutionState::DONE, res};
}

Result QueryResultCursor::dumpSync(VPackBuilder& builder) {
  try {
    size_t const n = batchSize();
    // reserve an arbitrary number of bytes for the result to save
    // some reallocs
    // (not accurate, but the actual size is unknown anyway)
    builder.reserve(std::max<size_t>(1, std::min<size_t>(n, 10000)) * 32);
    builder.add("result", VPackValue(VPackValueType::Array, true));
    for (size_t i = 0; i < n; ++i) {
      if (!hasNext()) {
        break;
      }
      builder.add(next());
    }
    builder.close();

    builder.add("hasMore", VPackValue(hasNext()));

    if (hasNext()) {
      builder.add("id", VPackValue(std::to_string(id())));
    }

    if (hasCount()) {
      builder.add("count", VPackValue(static_cast<uint64_t>(count())));
    }

    if (_result.extra != nullptr) {
      builder.add("extra", _result.extra->slice());
    }

    builder.add("cached", VPackValue(_cached));

    handleNextBatchIdValue(builder, hasNext());

    if (!hasNext() && !isRetriable()) {
      // mark the cursor as deleted
      this->setDeleted();
    }
  } catch (arangodb::basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL,
                  "internal error during QueryResultCursor::dump");
  }
  return {};
}

// .............................................................................
// QueryStreamCursor class
// .............................................................................

QueryStreamCursor::QueryStreamCursor(
    std::shared_ptr<arangodb::aql::Query> q, size_t batchSize, double ttl,
    bool isRetriable, transaction::OperationOrigin operationOrigin,
    std::shared_ptr<QueryAborter> aborter)
    : Cursor(TRI_NewServerSpecificTick(), batchSize, ttl, /*hasCount*/ false,
             isRetriable),
      _query(std::move(q)),
      _queryResultPos(0),
      _finalization(false),
      _allowDirtyReads(false) {
  _query->prepareQuery(aborter);
  _allowDirtyReads = _query->allowDirtyReads();  // is set by prepareQuery!
  TRI_IF_FAILURE("QueryStreamCursor::directKillAfterPrepare") {
    QueryStreamCursor::debugKillQuery();
  }

  // In all the following ASSERTs it is valid (though unlikely) that the query
  // is already killed In the cluster this kill operation will trigger cleanup
  // side-effects, such as changing the STATE and commiting / aborting the
  // transaction here
  TRI_ASSERT(_query->state() ==
                 aql::QueryExecutionState::ValueType::EXECUTION ||
             _query->killed());
  _ctx = _query->newTrxContext();

  transaction::Methods trx(_ctx);
  TRI_IF_FAILURE("QueryStreamCursor::directKillAfterTrxSetup") {
    QueryStreamCursor::debugKillQuery();
  }
  TRI_ASSERT(trx.status() == transaction::Status::RUNNING || _query->killed());

  // ensures the cursor is cleaned up as soon as the outer transaction ends
  // otherwise we just get issues because we might still try to use the trx
  TRI_ASSERT(trx.status() == transaction::Status::RUNNING || _query->killed());
  // things break if the Query outlives a V8 transaction
  _stateChangeCb = [this](transaction::Methods& /*trx*/,
                          transaction::Status status) {
    if (status == transaction::Status::COMMITTED ||
        status == transaction::Status::ABORTED) {
      this->setDeleted();
    }
  };
  if (!trx.addStatusChangeCallback(&_stateChangeCb)) {
    _stateChangeCb = nullptr;
  }

  _query->exitV8Executor();
}

QueryStreamCursor::~QueryStreamCursor() {
  if (!_query) {
    return;
  }

  _queryResults.clear();

  cleanupStateCallback();

  // Query destructor will cleanup plan and abort transaction
  _query.reset();
}

void QueryStreamCursor::kill() {
  if (_query) {
    _query->kill();
  }
}

void QueryStreamCursor::debugKillQuery() {
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  if (_query) {
    _query->debugKillQuery();
  }
#endif
}

uint64_t QueryStreamCursor::memoryUsage() const noexcept {
  // while a stream AQL query is operating, its memory usage
  // is tracked by the still-running query. the cursor does
  // not use a lot of memory on its own.
  uint64_t value = 2048 /* arbitrary fixed size value */;
  return value;
}

std::pair<ExecutionState, Result> QueryStreamCursor::dump(
    VPackBuilder& builder) {
  TRI_IF_FAILURE("QueryCursor::directKillBeforeQueryIsGettingDumped") {
    debugKillQuery();
  }

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  ScopeGuard sg([&]() noexcept {
    TRI_IF_FAILURE("QueryCursor::directKillAfterQueryIsGettingDumped") {
      debugKillQuery();
    }
  });
#endif

  TRI_ASSERT(batchSize() > 0);
  LOG_TOPIC("9af59", TRACE, Logger::QUERIES)
      << "executing query " << _id << ": '"
      << _query->queryString().extract(1024) << "'";

  auto guard = scopeGuard([&]() noexcept {
    try {
      if (_query) {
        _query->exitV8Executor();
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("a2bf8", ERR, Logger::QUERIES)
          << "Failed to exit V8 context: " << ex.what();
    }
  });

  try {
    ExecutionState state = prepareDump();
    if (state == ExecutionState::WAITING) {
      return {ExecutionState::WAITING, TRI_ERROR_NO_ERROR};
    }

    // writeResult will perform async finalize
    return {writeResult(builder), TRI_ERROR_NO_ERROR};
  } catch (arangodb::basics::Exception const& ex) {
    this->setDeleted();
    return {
        ExecutionState::DONE,
        Result(ex.code(), absl::StrCat("AQL: ", ex.message(),
                                       QueryExecutionState::toStringWithPrefix(
                                           _query->state())))};
  } catch (std::bad_alloc const&) {
    this->setDeleted();
    return {ExecutionState::DONE,
            Result(TRI_ERROR_OUT_OF_MEMORY,
                   absl::StrCat(TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                                QueryExecutionState::toStringWithPrefix(
                                    _query->state())))};
  } catch (std::exception const& ex) {
    this->setDeleted();
    return {
        ExecutionState::DONE,
        Result(TRI_ERROR_INTERNAL,
               absl::StrCat(ex.what(), QueryExecutionState::toStringWithPrefix(
                                           _query->state())))};
  } catch (...) {
    this->setDeleted();
    return {ExecutionState::DONE,
            Result(TRI_ERROR_INTERNAL,
                   absl::StrCat(TRI_errno_string(TRI_ERROR_INTERNAL),
                                QueryExecutionState::toStringWithPrefix(
                                    _query->state())))};
  }
}

Result QueryStreamCursor::dumpSync(VPackBuilder& builder) {
  TRI_IF_FAILURE("QueryCursor::directKillBeforeQueryIsGettingDumpedSynced") {
    debugKillQuery();
  }
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  ScopeGuard sg([&]() noexcept {
    TRI_IF_FAILURE("QueryCursor::directKillAfterQueryIsGettingDumpedSynced") {
      debugKillQuery();
    }
  });
#endif
  TRI_ASSERT(batchSize() > 0);
  LOG_TOPIC("9dada", TRACE, Logger::QUERIES)
      << "executing query " << _id << ": '"
      << _query->queryString().extract(1024) << "'";

  std::shared_ptr<SharedQueryState> ss = _query->sharedState();
  ss->resetWakeupHandler();

  auto guard = scopeGuard([&]() noexcept {
    try {
      if (_query) {
        _query->exitV8Executor();
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("db997", ERR, Logger::QUERIES)
          << "Failed to exit V8 context: " << ex.what();
    }
  });

  try {
    aql::ExecutionEngine* engine = _query->rootEngine();
    TRI_ASSERT(engine != nullptr);

    ExecutionState state = ExecutionState::WAITING;
    while (state == ExecutionState::WAITING) {
      state = prepareDump();
      if (state == ExecutionState::WAITING) {
        ss->waitForAsyncWakeup();
      }
    }

    state = ExecutionState::WAITING;
    while (state == ExecutionState::WAITING) {
      state = writeResult(builder);
      if (state == ExecutionState::WAITING) {
        ss->waitForAsyncWakeup();
      }
    }

  } catch (arangodb::basics::Exception const& ex) {
    this->setDeleted();
    return Result(
        ex.code(),
        absl::StrCat("AQL: ", ex.message(),
                     QueryExecutionState::toStringWithPrefix(_query->state())));
  } catch (std::bad_alloc const&) {
    this->setDeleted();
    return Result(
        TRI_ERROR_OUT_OF_MEMORY,
        absl::StrCat(TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY),
                     QueryExecutionState::toStringWithPrefix(_query->state())));
  } catch (std::exception const& ex) {
    this->setDeleted();
    return Result(
        TRI_ERROR_INTERNAL,
        absl::StrCat(ex.what(),
                     QueryExecutionState::toStringWithPrefix(_query->state())));
  } catch (...) {
    this->setDeleted();
    return Result(
        TRI_ERROR_INTERNAL,
        absl::StrCat(TRI_errno_string(TRI_ERROR_INTERNAL),
                     QueryExecutionState::toStringWithPrefix(_query->state())));
  }

  return Result();
}

/// Set wakeup callback on streaming cursor
void QueryStreamCursor::setWakeupHandler(std::function<bool()> const& cb) {
  if (_query) {
    _query->sharedState()->setWakeupHandler(cb);
  }
}

void QueryStreamCursor::resetWakeupHandler() {
  if (_query) {
    _query->sharedState()->resetWakeupHandler();
  }
}

ExecutionState QueryStreamCursor::writeResult(VPackBuilder& builder) {
  ResourceMonitor& resourceMonitor = _query->resourceMonitor();

  // this is here to track _temporary_ memory usage during result
  // building.
  ResourceUsageScope guard(resourceMonitor);
  auto const& buffer = builder.bufferRef();

  bool const silent = _query->queryOptions().silent;

  // reserve some space in Builder to avoid frequent reallocs
  if (!silent) {
    constexpr uint64_t reserveSize = 16 * 1024;
    // track memory usage
    guard.increase(reserveSize);
    // reserve the actual memory
    builder.reserve(reserveSize);
  }

  builder.add("result", VPackValue(VPackValueType::Array, true));

  aql::ExecutionEngine* engine = _query->rootEngine();
  TRI_ASSERT(engine != nullptr);
  // this is the RegisterId our results can be found in
  RegisterId const resultRegister = engine->resultRegister();

  size_t rowsWritten = 0;
  auto const& vopts = _query->vpackOptions();
  while (rowsWritten < batchSize() && !_queryResults.empty()) {
    SharedAqlItemBlockPtr& block = _queryResults.front();
    TRI_ASSERT(_queryResultPos < block->numRows());

    while (rowsWritten < batchSize() && _queryResultPos < block->numRows()) {
      if (!silent && resultRegister.isValid()) {
        AqlValue const& value =
            block->getValueReference(_queryResultPos, resultRegister);
        if (!value.isEmpty()) {  // ignore empty blocks (e.g. from UpdateBlock)
          uint64_t oldCapacity = buffer.capacity();

          value.toVelocyPack(&vopts, builder, /*allowUnindexed*/ true);
          ++rowsWritten;

          // track memory usage
          guard.increase(buffer.capacity() - oldCapacity);
        }
      }
      ++_queryResultPos;
    }

    if (_queryResultPos == block->numRows()) {
      // get next block
      TRI_ASSERT(_queryResultPos == block->numRows());
      _queryResults.pop_front();
      _queryResultPos = 0;
    }
  }

  TRI_ASSERT(_queryResults.empty() ||
             _queryResultPos < _queryResults.front()->numRows());

  builder.close();  // result

  // If there is a block left, there's at least one row left in it. On the
  // other hand, we rely on the caller to have fetched more than batchSize()
  // result rows if possible!
  bool const hasMore = !_queryResults.empty();

  builder.add("hasMore", VPackValue(hasMore));
  if (hasMore) {
    builder.add("id", VPackValue(std::to_string(id())));
  }
  handleNextBatchIdValue(builder, hasMore);

  builder.add("cached", VPackValue(false));

  if (!hasMore) {
    TRI_ASSERT(!_extrasBuffer.empty());
    builder.add("extra", VPackSlice(_extrasBuffer.data()));

    if (!isRetriable()) {
      // very important here, because _query may become invalid after here!
      guard.revert();

      _query.reset();
      this->setDeleted();
    }
    return ExecutionState::DONE;
  }

  return ExecutionState::HASMORE;
}

std::shared_ptr<transaction::Context> QueryStreamCursor::context() const {
  TRI_ASSERT(_ctx != nullptr);
  return _ctx;
}

ExecutionState QueryStreamCursor::prepareDump() {
  if (_finalization) {
    return finalization();
  }

  aql::ExecutionEngine* engine = _query->rootEngine();
  TRI_ASSERT(engine != nullptr);

  size_t numRows = 0;
  for (auto const& it : _queryResults) {
    numRows += it->maxModifiedRowIndex();
  }
  numRows -= _queryResultPos;

  // We want to fill a result of batchSize if possible and have at least
  // one row left (or be definitively DONE) to set "hasMore" reliably.
  ExecutionState state = ExecutionState::HASMORE;
  SkipResult skipped;
  bool const silent = _query->queryOptions().silent;

  AqlCall call;
  call.softLimit = batchSize();
  AqlCallList callList{std::move(call)};
  AqlCallStack callStack{std::move(callList)};

  while (state != ExecutionState::DONE && (silent || numRows <= batchSize())) {
    SharedAqlItemBlockPtr resultBlock;
    std::tie(state, skipped, resultBlock) = engine->execute(callStack);
    TRI_ASSERT(skipped.nothingSkipped());
    if (state == ExecutionState::WAITING) {
      break;
    }
    // resultBlock == nullptr => ExecutionState::DONE
    TRI_ASSERT(resultBlock != nullptr || state == ExecutionState::DONE);

    if (resultBlock != nullptr) {
      numRows += resultBlock->maxModifiedRowIndex();
      _queryResults.push_back(std::move(resultBlock));
    }
  }

  // remember not to call execute() again
  _finalization = (state == ExecutionState::DONE);
  if (_finalization) {
    cleanupStateCallback();
    return finalization();
  }

  return state;
}

ExecutionState QueryStreamCursor::finalization() {
  ExecutionState state = ExecutionState::DONE;
  if (_extrasBuffer.empty()) {
    VPackBuilder b(_extrasBuffer);
    // finalize(..) will commit transaction
    if ((state = _query->finalize(b)) == ExecutionState::WAITING) {
      return ExecutionState::WAITING;
    }
    TRI_ASSERT(b.slice().isObject());
  }
  TRI_ASSERT(state == ExecutionState::DONE);
  return state;
}

void QueryStreamCursor::cleanupStateCallback() {
  TRI_ASSERT(_query);
  transaction::Methods trx(_ctx);
  if (_stateChangeCb && trx.status() == transaction::Status::RUNNING) {
    trx.removeStatusChangeCallback(&_stateChangeCb);
    _stateChangeCb = nullptr;
  }
}
