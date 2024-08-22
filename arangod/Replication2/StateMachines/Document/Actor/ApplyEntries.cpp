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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#include "ApplyEntries.h"

#include <memory>
#include <optional>
#include <type_traits>

#include "Actor/LocalActorPID.h"
#include "Actor/LocalRuntime.h"

#include "Assertions/ProdAssert.h"

#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/StateMachines/Document/Actor/Transaction.h"
#include "Replication2/StateMachines/Document/DocumentStateShardHandler.h"
#include "Replication2/StateMachines/Document/LowestSafeIndexesForReplayUtils.h"
#include "Replication2/StateMachines/Document/ReplicatedOperation.h"
#include "Transaction/Methods.h"
#include "Transaction/OperationOrigin.h"

namespace arangodb::replication2::replicated_state::document::actor {

using namespace arangodb::actor;

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::operator()(
    message::ApplyEntries&& msg) noexcept
    -> std::unique_ptr<ApplyEntriesState> {
  ADB_PROD_ASSERT(this->state->_batch == nullptr);
  this->state->_batch = std::make_unique<ApplyEntriesState::Batch>(
      std::move(msg.entries), std::move(msg.promise));
  continueBatch();
  return std::move(this->state);
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::operator()(message::Resign&& msg) noexcept
    -> std::unique_ptr<ApplyEntriesState> {
  LOG_CTX("b0788", DEBUG, this->state->loggerContext)
      << "AppliesEntry actor received resign message";
  // We have to explicitly finish all started transaction actors.
  // This is necessary because we have a potential race. The DocumentState can
  // call softShutdown while we still process some entries. In this case we can
  // spawn a new actor after softShutdown has been called, and this actor will
  // never be finished
  for (auto& [tid, pid] : this->state->_transactionMap) {
    this->runtime().finishActor(pid, ExitReason::kShutdown);
  }
  resign();
  this->finish(ExitReason::kFinished);
  return std::move(this->state);
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::operator()(
    arangodb::actor::message::ActorDown<typename Runtime::ActorPID>&&
        msg) noexcept -> std::unique_ptr<ApplyEntriesState> {
  LOG_CTX("56a21", DEBUG, this->state->loggerContext)
      << "applyEntries actor received actor down message "
      << inspection::json(msg);
  if (msg.reason != ExitReason::kShutdown) {
    ADB_PROD_ASSERT(this->state->_pendingTransactions.contains(msg.actor))
        << inspection::json(this->state) << " msg " << inspection::json(msg);
    ADB_PROD_ASSERT(msg.reason == ExitReason::kFinished)
        << inspection::json(msg);
  }
  auto it = this->state->_pendingTransactions.find(msg.actor);
  ADB_PROD_ASSERT(it != this->state->_pendingTransactions.end() ||
                  msg.reason == ExitReason::kShutdown)
      << "received down message for unknown actor "
      << inspection::json(this->state) << " msg " << inspection::json(msg);

  if (it != this->state->_pendingTransactions.end()) {
    if (!it->second.intermediateCommit) {
      this->state->activeTransactions.markAsInactive(it->second.tid);
      // this transaction has finished, so we can remove it from the
      // transaction handler
      // normally this is already done when the transaction is committed or
      // aborted, but in case the transaction is broken and all operations are
      // skipped, we need to remove it here
      // For details about this special case see the Transaction actor
      this->state->followerState._transactionHandler->removeTransaction(
          it->second.tid);
    }
    this->state->_pendingTransactions.erase(it);
    if (this->state->_pendingTransactions.empty() && this->state->_batch) {
      // all pending trx finished, so we can now continuing processing the
      // batch
      continueBatch();
    }
  }
  return std::move(this->state);
}

template<typename Runtime>
void ApplyEntriesHandler<Runtime>::resign() {
  if (this->state->_batch) {
    resolveBatch(
        Result{TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED});
  }
}

template<typename Runtime>
void ApplyEntriesHandler<Runtime>::resolveBatch(Result result) {
  if (result.ok()) {
    std::optional<LogIndex> releaseIdx;
    if (this->state->_batch->lastIndex.has_value()) {
      releaseIdx = this->state->activeTransactions.getReleaseIndex().value_or(
          this->state->_batch->lastIndex.value());
    }
    this->state->_batch->promise.setValue(releaseIdx);
  } else {
    this->state->_batch->promise.setValue(result);
  }
  this->state->_batch.reset();
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::processEntry(DataDefinition auto& op,
                                                LogIndex index)
    -> ResultT<ProcessResult> {
  if (not this->state->_pendingTransactions.empty()) {
    return ProcessResult::kWaitForPendingTrx;
  }
  auto res = applyDataDefinitionEntry(op, index);
  if (res.fail()) {
    return res;
  }
  this->state->_batch->lastIndex = index;
  return ProcessResult::kContinue;
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::processEntry(
    ReplicatedOperation::AbortAllOngoingTrx& op, LogIndex index)
    -> ResultT<ProcessResult> {
  if (!this->state->_transactionMap.empty()) {
    // if we have active transactions finish them and add them to the list of
    // pending transactions
    for (auto& [tid, pid] : this->state->_transactionMap) {
      this->state->_pendingTransactions.emplace(
          pid, ApplyEntriesState::TransactionInfo{.tid = tid,
                                                  .intermediateCommit = false});
      this->runtime().finishActor(pid, ExitReason::kFinished);
    }
    this->state->_transactionMap.clear();
  }

  if (not this->state->_pendingTransactions.empty()) {
    return ProcessResult::kWaitForPendingTrx;
  }
  auto originalRes =
      this->state->followerState._transactionHandler->applyEntry(op);
  auto res =
      this->state->followerState._errorHandler->handleOpResult(op, originalRes);
  if (res.fail()) {
    return res;
  }

  this->state->_batch->lastIndex = index;
  this->state->activeTransactions.clear();
  return ProcessResult::kContinue;
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::processEntry(UserTransaction auto& op,
                                                LogIndex index)
    -> ResultT<ProcessResult> {
  using OpType = std::remove_cvref_t<decltype(op)>;
  LocalActorPID pid;
  auto it = this->state->_transactionMap.find(op.tid);
  if (it != this->state->_transactionMap.end()) {
    pid = it->second;
  } else {
    pid = this->template spawn<actor::TransactionActor>(
        std::make_unique<actor::TransactionState>(
            this->state->loggerContext,
            this->state->followerState._transactionHandler,
            this->state->followerState._errorHandler, op.tid));
    LOG_CTX("8a74c", DEBUG, this->state->loggerContext)
        << "spawned transaction actor " << pid.id << " for trx " << op.tid;
    this->monitor(pid);
    this->state->_transactionMap.emplace(op.tid, pid);
  }

  if (not beforeApplyEntry(op, index)) {
    // if beforeApplyEntry returns false, we can simply skip this entry
    // this is not an error!
    return ResultT<ProcessResult>::success(ProcessResult::kContinue);
  }

  constexpr bool isIntermediateCommit =
      std::is_same_v<OpType, ReplicatedOperation::IntermediateCommit>;
  if constexpr (FinishesUserTransaction<OpType> || isIntermediateCommit) {
    // this is either a commit or an abort - we remove the transaction from the
    // active transaction map and instead insert it in the pending transactions,
    // so other operations can wait for it to finish
    // Note: we handle intermediate commits the same way as regular commits,
    // because subsequent operations that belong to the same transaction will
    // simply start a new transaction actor with the same transaction id
    this->state->_transactionMap.erase(op.tid);
    this->state->_pendingTransactions.emplace(
        pid, ApplyEntriesState::TransactionInfo{
                 .tid = op.tid, .intermediateCommit = isIntermediateCommit});
  }

  if constexpr (FinishesUserTransaction<OpType>) {
    this->state->_batch->lastIndex = index;
  }

  this->template dispatch<message::TransactionMessages>(
      pid, message::ProcessEntry{.op = std::move(op), .index = index});

  if constexpr (FinishesUserTransaction<OpType> || isIntermediateCommit) {
    // we need to wait for the transaction to be committed before we can
    // continue
    // TODO - once we have proper dependency tracking in place this can be
    // relaxed
    return ResultT<ProcessResult>::success(
        ProcessResult::kMoveToNextEntryAndWaitForPendingTrx);
  } else {
    return ResultT<ProcessResult>::success(ProcessResult::kContinue);
  }
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::applyDataDefinitionEntry(
    ReplicatedOperation::DropShard const& op, LogIndex index) -> Result {
  // We first have to abort all transactions for this shard.
  // Note that after the entry is committed, locally all transactions on the
  // leader for this shard will be aborted. This will also add log entries to
  // abort these transactions; that is unnecessary, and we might want to avoid
  // it in the future. However, it doesn't hurt, so for now it's low on the
  // priority list.
  for (auto const& tid :
       this->state->followerState._transactionHandler->getTransactionsForShard(
           op.shard)) {
    auto abortRes = this->state->followerState._transactionHandler->applyEntry(
        ReplicatedOperation::Abort{tid});
    if (abortRes.fail()) {
      LOG_CTX("aa36c", INFO, this->state->loggerContext)
          << "Failed to abort transaction " << tid << " for shard " << op.shard
          << " before dropping the shard: " << abortRes.errorMessage();
      return abortRes;
    }
    this->state->activeTransactions.markAsInactive(tid);
  }
  return applyEntryAndReleaseIndex(op, index);
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::applyDataDefinitionEntry(
    ReplicatedOperation::ModifyShard const& op, LogIndex index) -> Result {
  // Note that locking the shard is not necessary on the follower.
  // However, we still do it for safety reasons.
  auto origin =
      transaction::OperationOriginREST{"follower collection properties update"};
  auto trxLock = this->state->followerState._shardHandler->lockShard(
      op.shard, AccessMode::Type::EXCLUSIVE, std::move(origin));
  if (trxLock.fail()) {
    auto res = this->state->followerState._errorHandler->handleOpResult(
        op, trxLock.result());

    // If the shard was not found, we can ignore this operation and release it.
    if (res.ok()) {
      return {};
    }

    return res;
  }
  return applyEntryAndReleaseIndex(op, index);
}

template<typename Runtime>
template<class T>
requires IsAnyOf<T, ReplicatedOperation::CreateShard,
                 ReplicatedOperation::CreateIndex,
                 ReplicatedOperation::DropIndex>
auto ApplyEntriesHandler<Runtime>::applyDataDefinitionEntry(T const& op,
                                                            LogIndex index)
    -> Result {
  return applyEntryAndReleaseIndex(op, index);
}

template<typename Runtime>
template<class T>
auto ApplyEntriesHandler<Runtime>::applyEntryAndReleaseIndex(T const& op,
                                                             LogIndex index)
    -> Result {
  auto originalRes = std::invoke([&]() {
    if constexpr (std::is_same_v<T, ReplicatedOperation::CreateIndex>) {
      // all entries until here have already been applied; there are no
      // open transactions; it is safe to increase the lowest safe index
      // now. Then we can create the index.
      auto lowestSafeIndexesForReplayGuard =
          this->state->followerState._lowestSafeIndexesForReplay
              .getLockedGuard();
      return this->state->followerState._transactionHandler->applyEntry(
          op, index, lowestSafeIndexesForReplayGuard.get(),
          *this->state->followerState.getStream());
    } else {
      return this->state->followerState._transactionHandler->applyEntry(op);
    }
  });
  auto res =
      this->state->followerState._errorHandler->handleOpResult(op, originalRes);
  if (res.fail()) {
    return res;
  }
  return {};
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::beforeApplyEntry(
    ModifiesUserTransaction auto const& op, LogIndex index) -> bool {
  auto lowestSafeIndexesForReplayGuard =
      this->state->followerState._lowestSafeIndexesForReplay.getLockedGuard();
  bool const isSafeForReplay =
      lowestSafeIndexesForReplayGuard->isSafeForReplay(op.shard, index);
  if (isSafeForReplay) {
    this->state->activeTransactions.markAsActive(op.tid, index);
  }
  return isSafeForReplay;
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::beforeApplyEntry(
    ReplicatedOperation::IntermediateCommit const& op, LogIndex) -> bool {
  if (!this->state->activeTransactions.getTransactions().contains(op.tid)) {
    LOG_CTX("b41dc", INFO, this->state->loggerContext)
        << "will not apply intermediate commit for transaction " << op.tid
        << " because it is not active";
    return false;
  }
  return true;
}

template<typename Runtime>
auto ApplyEntriesHandler<Runtime>::beforeApplyEntry(
    FinishesUserTransaction auto const& op, LogIndex index) -> bool {
  if (!this->state->activeTransactions.getTransactions().contains(op.tid)) {
    // Single commit/abort operations are possible.
    LOG_CTX("cf7ea", INFO, this->state->loggerContext)
        << "will not finish transaction " << op.tid
        << " because it is not active";
    return false;
  }
  return true;
}

template<typename Runtime>
void ApplyEntriesHandler<Runtime>::continueBatch() noexcept try {
  ADB_PROD_ASSERT(this->state->_batch != nullptr);
  ADB_PROD_ASSERT(this->state->_pendingTransactions.empty());
  if (not this->state->_batch->_currentEntry.has_value()) {
    resolveBatch(Result{});
    return;
  }

  do {
    auto& e = this->state->_batch->_currentEntry.value();
    LogIndex index = e.first;
    auto& doc = e.second;

    auto res = std::visit(
        [&](auto&& op) -> ResultT<ProcessResult> {
          return processEntry(op, index);
        },
        doc.getInnerOperation());

    if (res.fail()) {
      TRI_ASSERT(this->state->followerState._errorHandler
                     ->handleOpResult(doc.getInnerOperation(), res.result())
                     .fail())
          << res.result() << " should have been already handled for operation "
          << doc.getInnerOperation() << " during applyEntries of follower "
          << this->state->loggerContext;
      LOG_CTX("0aa2e", FATAL, this->state->loggerContext)
          << "failed to apply entry " << doc
          << " on follower: " << res.result();
      TRI_ASSERT(false) << res.result();
      FATAL_ERROR_EXIT();
    }

    if (res.get() == ProcessResult::kWaitForPendingTrx) {
      ADB_PROD_ASSERT(not this->state->_pendingTransactions.empty());
      // the current entry requires all pending transactions to have finished,
      // so we return here and wait for the transactions's finish message before
      // we continue
      return;
    }

    this->state->_batch->_currentEntry = this->state->_batch->entries->next();
    if (res.get() == ProcessResult::kMoveToNextEntryAndWaitForPendingTrx) {
      // we successfully processed the last entry and moved on, but the last
      // entry indicated that we have to wait for pending transactions to finish
      // before processing the next entry, so we return here and wait for the
      // transactions's finish message before we continue
      ADB_PROD_ASSERT(not this->state->_pendingTransactions.empty());
      return;
    }
  } while (this->state->_batch->_currentEntry.has_value());

  if (this->state->_pendingTransactions.empty()) {
    resolveBatch(Result{});
  }
  // we have processed all entries, but there are still pending transactions
  // that we need to wait for, before we can resolve the batch
} catch (std::exception const& ex) {
  LOG_CTX("3927b", FATAL, this->state->loggerContext)
      << "Caught an exception when applying entries. This is fatal - the "
         "process will terminate now. The exception was: "
      << ex.what();
  FATAL_ERROR_ABORT();
} catch (...) {
  LOG_CTX("d57fc", FATAL, this->state->loggerContext)
      << "Caught an unknown exception when applying entries. This is fatal - "
         "the process will terminate now.";
  FATAL_ERROR_ABORT();
}

template struct ApplyEntriesHandler<LocalRuntime>;

}  // namespace arangodb::replication2::replicated_state::document::actor
