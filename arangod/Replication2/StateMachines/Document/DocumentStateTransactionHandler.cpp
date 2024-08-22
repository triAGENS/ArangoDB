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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#include "Replication2/StateMachines/Document/DocumentStateTransactionHandler.h"

#include "Basics/application-exit.h"
#include "Basics/voc-errors.h"
#include "Logger/LogContextKeys.h"
#include "Replication2/StateMachines/Document/DocumentStateHandlersFactory.h"
#include "Replication2/StateMachines/Document/DocumentStateShardHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateTransaction.h"
#include "Replication2/StateMachines/Document/LowestSafeIndexesForReplayUtils.h"
#include "VocBase/AccessMode.h"

namespace arangodb::replication2::replicated_state::document {

DocumentStateTransactionHandler::DocumentStateTransactionHandler(
    GlobalLogIdentifier gid, TRI_vocbase_t* vocbase,
    std::shared_ptr<IDocumentStateHandlersFactory> factory,
    std::shared_ptr<IDocumentStateShardHandler> shardHandler)
    : _gid(std::move(gid)),
      _vocbase(vocbase),
      _loggerContext(LoggerContext(Logger::REPLICATED_STATE)
                         .with<logContextKeyDatabaseName>(_gid.database)
                         .with<logContextKeyLogId>(_gid.id)),
      _factory(std::move(factory)),
      _shardHandler(std::move(shardHandler)),
      _errorHandler(_factory->createErrorHandler(_gid)) {
#ifndef ARANGODB_USE_GOOGLE_TESTS
  TRI_ASSERT(_vocbase != nullptr);
#endif
}

auto DocumentStateTransactionHandler::getTrx(TransactionId tid)
    -> std::shared_ptr<IDocumentStateTransaction> {
  return _transactions.doUnderLock(
      [&](auto& transactions) -> std::shared_ptr<IDocumentStateTransaction> {
        auto it = transactions.find(tid);
        if (it == transactions.end()) {
          return nullptr;
        }
        return it->second;
      });
}

void DocumentStateTransactionHandler::setTrx(
    TransactionId tid, std::shared_ptr<IDocumentStateTransaction> trx) {
  auto [_, isInserted] =
      _transactions.getLockedGuard()->emplace(tid, std::move(trx));
  ADB_PROD_ASSERT(isInserted)
      << "Transaction " << tid << " already exists (gid " << _gid << ")";
}

void DocumentStateTransactionHandler::removeTransaction(TransactionId tid) {
  _transactions.getLockedGuard()->erase(tid);
}

auto DocumentStateTransactionHandler::getUnfinishedTransactions() const
    -> TransactionMap {
  return _transactions.copy();
}

auto DocumentStateTransactionHandler::getTransactionsForShard(
    ShardID const& sid) -> std::vector<TransactionId> {
  std::vector<TransactionId> result;
  _transactions.doUnderLock([&](auto& transactions) {
    for (auto const& [tid, trx] : transactions) {
      if (trx->containsShard(sid)) {
        result.emplace_back(tid);
      }
    }
  });
  return result;
}

auto DocumentStateTransactionHandler::applyOp(
    FinishesUserTransaction auto const& op) -> Result {
  TRI_ASSERT(op.tid.isFollowerTransactionId());
  auto trx = getTrx(op.tid);
  ADB_PROD_ASSERT(trx != nullptr)
      << "Transaction " << op.tid << " not found for operation " << op
      << " (gid " << _gid << ")";

  auto res = std::invoke(
      overload{
          [&](ReplicatedOperation::Commit const&) { return trx->commit(); },
          [&](ReplicatedOperation::Abort const&) { return trx->abort(); }},
      op);

  removeTransaction(op.tid);
  return res;
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::IntermediateCommit const& op) -> Result {
  TRI_ASSERT(op.tid.isFollowerTransactionId());
  auto trx = getTrx(op.tid);
  ADB_PROD_ASSERT(trx != nullptr)
      << "Transaction " << op.tid << " not found for operation " << op
      << " (gid " << _gid << ")";
  return trx->intermediateCommit();
}

auto DocumentStateTransactionHandler::applyOp(
    ModifiesUserTransaction auto const& op) -> Result {
  TRI_ASSERT(op.tid.isFollowerTransactionId()) << op << " " << _gid;

  auto trx = getTrx(op.tid);
  if (trx == nullptr) {
    using T = std::decay_t<decltype(op)>;
    auto accessType = std::is_same_v<T, ReplicatedOperation::Truncate>
                          ? AccessMode::Type::EXCLUSIVE
                          : AccessMode::Type::WRITE;
    TRI_ASSERT(_vocbase != nullptr) << op << " " << _gid;
    trx = _factory->createTransaction(*_vocbase, op.tid, op.shard, accessType,
                                      op.userName);
    setTrx(op.tid, trx);
  }

  auto opRes = trx->apply(op);
  return _errorHandler->handleDocumentTransactionResult(opRes, op.tid);
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::AbortAllOngoingTrx const&) -> Result {
  _transactions.getLockedGuard()->clear();
  return {};
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::CreateShard const& op) -> Result {
  return _shardHandler->ensureShard(op.shard, op.collectionType, op.properties);
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::ModifyShard const& op) -> Result {
  return _shardHandler->modifyShard(op.shard, op.collection, op.properties);
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::DropShard const& op) -> Result {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // Make sure all transactions are aborted before dropping a shard.
  auto transactions = getTransactionsForShard(op.shard);
  TRI_ASSERT(transactions.empty())
      << "On follower " << _gid
      << " some transactions were not aborted before dropping shard "
      << op.shard << ": " << transactions;
#endif
  return _shardHandler->dropShard(op.shard);
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::CreateIndex const& op, LogIndex index,
    LowestSafeIndexesForReplay& lowestSafeIndexesForReplay,
    streams::Stream<DocumentState>& stream) -> Result {
  // all entries until here have already been applied; there are
  // no open transactions; it is safe to increase the lowest
  // safe index now. Then we can safely create the index.
  increaseAndPersistLowestSafeIndexForReplayTo(
      _loggerContext, lowestSafeIndexesForReplay, stream, op.shard, index);
  return _shardHandler->ensureIndex(op.shard, op.properties.slice(), nullptr,
                                    nullptr);
}

auto DocumentStateTransactionHandler::applyOp(
    ReplicatedOperation::DropIndex const& op) -> Result {
  return _shardHandler->dropIndex(op.shard, op.indexId);
}

template<typename Op, typename... Args>
auto DocumentStateTransactionHandler::applyAndCatchAndLog(Op&& op,
                                                          Args&&... args)
    -> Result {
  auto result = basics::catchToResult([&]() {
    return applyOp(std::forward<Op>(op), std::forward<Args>(args)...);
  });
  if (result.fail()) {
    LOG_CTX("01202", DEBUG, _loggerContext)
        << "Error occurred while applying operation " << op << " " << result
        << ". This is not necessarily a problem. Some errors are expected to "
           "occur during leader or follower recovery.";
  }
  return result;
}

Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Commit const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Abort const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::IntermediateCommit const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Truncate const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Insert const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Update const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Replace const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::Remove const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::AbortAllOngoingTrx const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::CreateShard const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::ModifyShard const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::DropShard const& op) noexcept {
  return applyAndCatchAndLog(op);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::CreateIndex const& op, LogIndex index,
    LowestSafeIndexesForReplay& lowestSafeIndexesForReplay,
    streams::Stream<DocumentState>& stream) noexcept {
  return applyAndCatchAndLog(op, index, lowestSafeIndexesForReplay, stream);
}
Result DocumentStateTransactionHandler::applyEntry(
    ReplicatedOperation::DropIndex const& op) noexcept {
  return applyAndCatchAndLog(op);
}

}  // namespace arangodb::replication2::replicated_state::document
