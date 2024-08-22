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

#include "Replication2/StateMachines/Document/DocumentLeaderState.h"

#include "Basics/application-exit.h"
#include "Replication2/ReplicatedState/StateInterfaces.tpp"
#include "Replication2/StateMachines/Document/DocumentFollowerState.h"
#include "Replication2/StateMachines/Document/DocumentLogEntry.h"
#include "Replication2/StateMachines/Document/DocumentStateErrorHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateHandlersFactory.h"
#include "Replication2/StateMachines/Document/DocumentStateShardHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateSnapshotHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateTransactionHandler.h"
#include "Replication2/StateMachines/Document/LowestSafeIndexesForReplayUtils.h"
#include "Transaction/Manager.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"

#include <Futures/Future.h>
#include <Logger/LogContextKeys.h>

namespace arangodb::replication2::replicated_state::document {

DocumentLeaderState::GuardedData::GuardedData(
    std::unique_ptr<DocumentCore> core,
    std::shared_ptr<IDocumentStateHandlersFactory> const& handlersFactory)
    : core(std::move(core)) {
  transactionHandler = handlersFactory->createTransactionHandler(
      this->core->getVocbase(), this->core->gid,
      handlersFactory->createShardHandler(this->core->getVocbase(),
                                          this->core->gid));
}

DocumentLeaderState::DocumentLeaderState(
    std::unique_ptr<DocumentCore> core, std::shared_ptr<Stream> stream,
    std::shared_ptr<IDocumentStateHandlersFactory> handlersFactory,
    transaction::IManager& transactionManager)
    : IReplicatedLeaderState(std::move(stream)),
      gid(core->gid),
      loggerContext(
          handlersFactory->createLogger(gid).with<logContextKeyStateComponent>(
              "LeaderState")),
      _handlersFactory(std::move(handlersFactory)),
      _shardHandler(
          _handlersFactory->createShardHandler(core->getVocbase(), gid)),
      _snapshotHandler(
          _handlersFactory->createSnapshotHandler(core->getVocbase(), gid)),
      _errorHandler(_handlersFactory->createErrorHandler(gid)),
      _guardedData(std::move(core), _handlersFactory),
      _transactionManager(transactionManager),
      _lowestSafeIndexesForReplay(getStream()->getCommittedMetadata()) {
  // Get ready to replay the log
  _shardHandler->prepareShardsForLogReplay();
}

auto DocumentLeaderState::resign() && noexcept
    -> std::unique_ptr<DocumentCore> {
  _resigning = true;

  auto core = _guardedData.doUnderLock([&](auto& data) {
    ADB_PROD_ASSERT(!data.didResign())
        << "resigning leader " << gid
        << " is not possible because it is already resigned";

    auto abortAllRes = data.transactionHandler->applyEntry(
        ReplicatedOperation::AbortAllOngoingTrx{});
    ADB_PROD_ASSERT(abortAllRes.ok()) << abortAllRes;

    return std::move(data.core);
  });

  auto snapshotsGuard = _snapshotHandler.getLockedGuard();
  if (auto& snapshotHandler = snapshotsGuard.get(); snapshotHandler) {
    snapshotHandler->clear();
    // The snapshot handler holds a reference to the underlying vocbase. It must
    // not be accessed after resign, so the database can be dropped safely.
    snapshotHandler.reset();
  } else {
    TRI_ASSERT(false) << "Double-resign in document leader state " << gid;
  }

  // We are taking a copy of active transactions to avoid a deadlock in doAbort.
  // doAbort is called within abortManagedTrx below, and requires it's own lock
  // on the guarded data.
  auto activeTransactions = _activeTransactions.copy().getTransactions();
  for (auto const& trx : activeTransactions) {
    auto tid = trx.first.asLeaderTransactionId();
    if (auto abortRes = basics::catchToResult([&]() {
          return _transactionManager.abortManagedTrx(tid, gid.database)
              .waitAndGet();
        });
        abortRes.fail()) {
      LOG_CTX("1b665", WARN, loggerContext)
          << "Failed to register tombstone for " << tid
          << " during resign: " << abortRes;
    } else {
      LOG_CTX("fb6d1", TRACE, loggerContext)
          << "Registered tombstone for " << tid << " during resign";
    }
  }

  return core;
}

auto DocumentLeaderState::recoverEntries(std::unique_ptr<EntryIterator> ptr)
    -> futures::Future<Result> {
  return _guardedData.doUnderLock([self = shared_from_this(),
                                   ptr = std::move(ptr)](
                                      auto& data) -> futures::Future<Result> {
    if (data.didResign()) {
      return {TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED};
    }

    // TODO should we just copy this and release the mutex?
    auto lowestSafeIndexesForReplayGuard =
        self->_lowestSafeIndexesForReplay.getLockedGuard();

    std::unordered_set<TransactionId> activeTransactions;
    while (auto entry = ptr->next()) {
      if (self->_resigning) {
        // We have not officially resigned yet, but we are about to. So, we can
        // just stop here.
        break;
      }
      auto [idx, doc] = *entry;

      bool const isSafeForReplay = std::visit(
          overload{
              // TODO do we have to handle other kinds of log entries?
              [&](ModifiesUserTransaction auto const& op) -> bool {
                return lowestSafeIndexesForReplayGuard->isSafeForReplay(
                    op.shard, idx);
              },
              [](auto const&) { return true; },
          },
          doc.getInnerOperation());

      if (isSafeForReplay) {
        auto res = std::visit(
            overload{
                [&](ModifiesUserTransaction auto const& op) -> Result {
                  auto trxResult = data.transactionHandler->applyEntry(op);
                  // Only add it as an active transaction if the operation
                  // was successful.
                  if (trxResult.ok()) {
                    activeTransactions.insert(op.tid);
                  }
                  return trxResult;
                },
                [&](FinishesUserTransactionOrIntermediate auto const& op)
                    -> Result {
                  // There are three cases where we can end up here:
                  // 1. After recovery, we did not get the beginning of the
                  // transaction.
                  // 2. We ignored all other operations for this
                  // transaction because the shard was dropped.
                  // 3. The transaction applies to multiple shards, which
                  // all had the same leader, thus multiple commits were
                  // replicated for the same transaction.
                  if (activeTransactions.erase(op.tid) == 0) {
                    return Result{};
                  }
                  return data.transactionHandler->applyEntry(op);
                },
                [&](ReplicatedOperation::DropShard const& op) -> Result {
                  // Abort all transactions for this shard.
                  for (auto const& tid :
                       data.transactionHandler->getTransactionsForShard(
                           op.shard)) {
                    activeTransactions.erase(tid);
                    auto abortRes = data.transactionHandler->applyEntry(
                        ReplicatedOperation::Abort{tid});
                    if (abortRes.fail()) {
                      LOG_CTX("3eb75", INFO, self->loggerContext)
                          << "failed to abort transaction " << tid
                          << " for shard " << op.shard
                          << " during recovery: " << abortRes;
                      return abortRes;
                    }
                  }
                  return data.transactionHandler->applyEntry(op);
                },
                [&](ReplicatedOperation::CreateIndex const& op) {
                  if (auto trxs =
                          data.transactionHandler->getTransactionsForShard(
                              op.shard);
                      not trxs.empty()) {
                    auto const name = [&] {
                      try {
                        return op.properties.get(StaticStrings::IndexName)
                            .stringView();
                      } catch (...) {
                      }
                      try {
                        return op.properties.get(StaticStrings::IndexId)
                            .stringView();
                      } catch (...) {
                      }
                      TRI_ASSERT(false);
                      return std::string_view("n/a");
                    }();
                    auto msg = std::stringstream{};
                    msg << "During recovery, when creating index " << name
                        << " on shard " << op.shard
                        << ", there are open transactions: " << trxs
                        << ". This must not happen, aborting recovery. Please "
                           "contact ArangoDB support.";
                    LOG_CTX("21517", ERR, self->loggerContext) << msg.str();
                    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                                   msg.str());
                  }
                  return data.transactionHandler->applyEntry(
                      op, idx, lowestSafeIndexesForReplayGuard.get(),
                      *self->getStream());
                },
                [&](auto const& op) {
                  return data.transactionHandler->applyEntry(op);
                }},
            doc.getInnerOperation());

        if (res.fail()) {
          if (self->_errorHandler->handleOpResult(doc.getInnerOperation(), res)
                  .fail()) {
            LOG_CTX("cbc5b", FATAL, self->loggerContext)
                << "failed to apply entry " << doc
                << " during recovery: " << res;
            TRI_ASSERT(false) << doc << " " << res;
            FATAL_ERROR_EXIT();
          }
        }
      } else {
        // TODO don't log the entire operation, including the payload!
        //      Also check other log statements for this problem.
        LOG_CTX("acdaa", TRACE, self->loggerContext)
            << "ignoring log entry [" << idx << "] " << doc
            << " during recovery: not safe for replay";
      }
    }
    auto abortAll = ReplicatedOperation::AbortAllOngoingTrx{};
    auto abortAllReplicationFut =
        self->replicateOperation(abortAll, ReplicationOptions{});
    // Should finish immediately, because we are not waiting the operation to be
    // committed in the replicated log
    TRI_ASSERT(abortAllReplicationFut.isReady()) << self->gid;
    auto abortAllReplicationStatus = abortAllReplicationFut.waitAndGet();
    if (abortAllReplicationStatus.fail()) {
      // The replicatedOperation call from above may fail if the leader has
      // resigned from the perspective of the framework. We should not panic, as
      // this is a harmless situation.
      if (!abortAllReplicationStatus.is(
              TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED)) {
        LOG_CTX("b4217", FATAL, self->loggerContext)
            << "failed to replicate AbortAllOngoingTrx operation during "
               "recovery: "
            << abortAllReplicationStatus.result();
        TRI_ASSERT(false) << abortAllReplicationStatus.result();
        FATAL_ERROR_EXIT();
      } else {
        LOG_CTX("9dd38", DEBUG, self->loggerContext)
            << "Failed to replicate AbortAllOngoingTrx operation during "
               "recovery because the leader has resigned already";
      }
    }

    for (auto& [trxId, trx] :
         data.transactionHandler->getUnfinishedTransactions()) {
      // The log entries contain follower ids, which is fine since during
      // recovery we apply the entries like a follower, but we have to
      // register tombstones in the trx managers for the leader trx id.
      auto tid = trxId.asLeaderTransactionId();
      if (auto abortRes = basics::catchToResult([&]() {
            return self->_transactionManager
                .abortManagedTrx(tid, self->gid.database)
                .waitAndGet();
          });
          abortRes.fail()) {
        LOG_CTX("462a9", DEBUG, self->loggerContext)
            << "Failed to register tombstone for " << tid
            << " during recovery: " << abortRes;
      }
    }

    // All unfinished transactions will be aborted
    auto abortAllTrxStatus = data.transactionHandler->applyEntry(abortAll);
    TRI_ASSERT(abortAllTrxStatus.ok()) << abortAllTrxStatus;
    if (abortAllTrxStatus.fail()) {
      LOG_CTX("f3f91", WARN, self->loggerContext)
          << "failed to abort all previous transactions during recovery: "
          << abortAllTrxStatus;
    }

    if (self->_resigning) {
      return {TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED};
    }

    if (auto releaseRes = self->release(abortAllReplicationStatus.get());
        releaseRes.fail()) {
      LOG_CTX("55140", DEBUG, self->loggerContext)
          << "Failed to call release during leader recovery: " << releaseRes;
    }

    return {TRI_ERROR_NO_ERROR};
  });
}

auto DocumentLeaderState::needsReplication(ReplicatedOperation const& op)
    -> bool {
  return std::visit(
      overload{
          [&](FinishesUserTransactionOrIntermediate auto const& op) -> bool {
            // An empty transaction is not active, therefore we can ignore it.
            // We are not replicating commit or abort operations for empty
            // transactions.
            return _activeTransactions.getLockedGuard()
                ->getTransactions()
                .contains(op.tid);
          },
          [&](auto&&) -> bool { return true; },
      },
      op.operation);
}

auto DocumentLeaderState::replicateOperation(ReplicatedOperation op,
                                             ReplicationOptions opts)
    -> futures::Future<ResultT<LogIndex>> {
  auto const& stream = getStream();

  auto entry = DocumentLogEntry(std::move(op));

  // Insert and emplace must happen atomically. The idx is strictly increasing,
  // which is required by markAsActive. Keeping the stream insertion and the
  // markAsActive call under the same lock ensures that we never call
  // markAsActive on a log index that is lower than the latest inserted one.
  auto&& insertionRes = basics::catchToResultT([&] {
    return _activeTransactions.doUnderLock([&](auto& activeTransactions) {
      auto idx = stream->insert(entry, opts.waitForSync);

      std::visit(overload{
                     [&](UserTransaction auto& op) {
                       // We have to use follower IDs when replicating
                       // transactions
                       TRI_ASSERT(op.tid.isFollowerTransactionId())
                           << op.tid
                           << " must only be replicated using a follower "
                              "transaction id "
                           << op;
                       activeTransactions.markAsActive(op.tid, idx);
                     },
                     [&](auto&&) { activeTransactions.markAsActive(idx); },
                 },
                 entry.getInnerOperation());

      return idx;
    });
  });

  if (insertionRes.fail()) {
    if (insertionRes.is(TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED)) {
      LOG_CTX("c8977", WARN, loggerContext)
          << "Will not insert operation into the stream because the leader "
             "resigned. During a failover, this might not be a problem.";
    } else {
      LOG_CTX("ffe2f", ERR, loggerContext)
          << "replicateOperation failed to insert into the stream: "
          << insertionRes.result();
    }
    return insertionRes.result();
  }
  auto idx = insertionRes.get();

  if (opts.waitForCommit) {
    try {
      return stream->waitFor(idx).thenValue([idx](auto&& result) {
        return futures::Future<ResultT<LogIndex>>{idx};
      });
    } catch (basics::Exception const& e) {
      return Result{e.code(), e.what()};
    } catch (std::exception const& e) {
      return Result{TRI_ERROR_INTERNAL, e.what()};
    }
  }

  return LogIndex{idx};
}

auto DocumentLeaderState::release(LogIndex index) -> Result {
  auto const& stream = getStream();
  return basics::catchToResult([&]() -> Result {
    auto idx = _activeTransactions.doUnderLock([&](auto& activeTransactions) {
      activeTransactions.markAsInactive(index);
      return activeTransactions.getReleaseIndex().value_or(index);
    });
    stream->release(idx);
    return {TRI_ERROR_NO_ERROR};
  });
}

auto DocumentLeaderState::release(TransactionId tid, LogIndex index) -> Result {
  auto const& stream = getStream();
  return basics::catchToResult([&]() -> Result {
    auto idx = _activeTransactions.doUnderLock([&](auto& activeTransactions) {
      activeTransactions.markAsInactive(tid);
      return activeTransactions.getReleaseIndex().value_or(index);
    });
    stream->release(idx);
    return {TRI_ERROR_NO_ERROR};
  });
}

template<class ResultType, class GetFunc, class ProcessFunc>
auto DocumentLeaderState::executeSnapshotOperation(GetFunc getSnapshot,
                                                   ProcessFunc processSnapshot)
    -> ResultType {
  // Fetch the snapshot
  auto snapshotRes = _snapshotHandler.doUnderLock(
      [&](auto& handler) -> ResultT<std::weak_ptr<Snapshot>> {
        if (handler == nullptr) {
          return Result{
              TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
              fmt::format(
                  "Could not get snapshot status of {}, state resigned.", gid)};
        }
        return getSnapshot(handler);
      });
  if (snapshotRes.fail()) {
    return snapshotRes.result();
  }

  // Do the necessary processing on it
  if (auto snapshot = snapshotRes.get().lock(); snapshot != nullptr) {
    return processSnapshot(snapshot);
  }

  return Result(TRI_ERROR_INTERNAL,
                fmt::format("Snapshot not available for {}!", gid));
}

auto DocumentLeaderState::snapshotStatus(SnapshotId id)
    -> ResultT<SnapshotStatus> {
  return executeSnapshotOperation<ResultT<SnapshotStatus>>(
      [&](auto& handler) { return handler->find(id); },
      [](auto& snapshot) { return snapshot->status(); });
}

auto DocumentLeaderState::snapshotStart(SnapshotParams::Start const& params)
    -> ResultT<SnapshotBatch> {
  return executeSnapshotOperation<ResultT<SnapshotBatch>>(
      [&](auto& handler) {
        return handler->create(_shardHandler->getAvailableShards(), params);
      },
      [](auto& snapshot) { return snapshot->fetch(); });
}

auto DocumentLeaderState::snapshotNext(SnapshotParams::Next const& params)
    -> ResultT<SnapshotBatch> {
  return executeSnapshotOperation<ResultT<SnapshotBatch>>(
      [&](auto& handler) { return handler->find(params.id); },
      [](auto& snapshot) { return snapshot->fetch(); });
}

auto DocumentLeaderState::snapshotFinish(const SnapshotParams::Finish& params)
    -> Result {
  return _snapshotHandler.doUnderLock([&](auto& handler) -> Result {
    if (handler == nullptr) {
      return Result{
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
          fmt::format("Could not get snapshot status of {}, state resigned.",
                      gid)};
    }
    return handler->finish(params.id);
  });
}

auto DocumentLeaderState::allSnapshotsStatus() -> ResultT<AllSnapshotsStatus> {
  return _snapshotHandler.doUnderLock(
      [&](auto& handler) -> ResultT<AllSnapshotsStatus> {
        if (handler) {
          return handler->status();
        }
        return Result{
            TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
            fmt::format(
                "Could not get snapshot statuses of {}, state resigned.",
                to_string(gid))};
      });
}

auto DocumentLeaderState::getAssociatedShardList() const
    -> std::vector<ShardID> {
  auto shards = _shardHandler->getAvailableShards();

  std::vector<ShardID> shardIds;
  shardIds.reserve(shards.size());
  for (auto&& shardId : shards) {
    auto maybeShardId = ShardID::shardIdFromString(shardId->name());
    ADB_PROD_ASSERT(maybeShardId.ok())
        << "Tried to produce shard list on Database Server for a collection "
           "that is not a shard "
        << shardId->name();
    shardIds.emplace_back(maybeShardId.get());
  }
  return shardIds;
}

auto DocumentLeaderState::createShard(ShardID shard,
                                      TRI_col_type_e collectionType,
                                      velocypack::SharedSlice properties)
    -> futures::Future<Result> {
  auto op = ReplicatedOperation::CreateShard{shard, collectionType,
                                             std::move(properties)};

  auto fut = _guardedData.doUnderLock([&](auto& data) {
    if (data.didResign()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
          fmt::format(
              "Leader {} resigned before replicating CreateShard operation",
              gid));
    }

    return replicateOperation(
        op, ReplicationOptions{.waitForCommit = true, .waitForSync = true});
  });

  return std::move(fut).thenValue([self = shared_from_this(),
                                   op = std::move(op)](auto&& result) mutable {
    if (result.fail()) {
      return result.result();
    }
    auto logIndex = result.get();

    return self->_guardedData.doUnderLock(
        [self, logIndex, op = std::move(op)](auto& data) -> Result {
          if (data.didResign()) {
            return TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED;
          }
          auto&& applyEntryRes = data.transactionHandler->applyEntry(op);

          // Some errors can be safely ignored, even though the operation has
          // been already replicated.
          if (auto res = self->_errorHandler->handleOpResult(op, applyEntryRes);
              res.fail()) {
            if (res.is(TRI_ERROR_SHUTTING_DOWN)) {
              LOG_CTX("e07a8", DEBUG, self->loggerContext)
                  << "CreateShard operation failed on the leader, due to the "
                     "server shutting down. CreateShard log entry will not be "
                     "released.";
              return TRI_ERROR_NO_ERROR;
            }

            LOG_CTX("d11f0", FATAL, self->loggerContext)
                << "CreateShard operation failed on the leader, after being "
                   "replicated to followers: "
                << applyEntryRes;
            TRI_ASSERT(false) << applyEntryRes;
            FATAL_ERROR_EXIT();
          }

          if (auto releaseRes = self->release(logIndex); releaseRes.fail()) {
            LOG_CTX("ed8e5", ERR, self->loggerContext)
                << "Failed to call release on index " << logIndex << ": "
                << releaseRes;
          }
          return applyEntryRes;
        });
  });
}

auto DocumentLeaderState::modifyShard(ShardID shard, CollectionID collectionId,
                                      velocypack::SharedSlice properties)
    -> futures::Future<Result> {
  auto origin =
      transaction::OperationOriginREST{"leader collection properties update"};
  auto trxLock = _shardHandler->lockShard(shard, AccessMode::Type::EXCLUSIVE,
                                          std::move(origin));
  if (trxLock.fail()) {
    return trxLock.result();
  } else if (trxLock.get() == nullptr) {
    TRI_ASSERT(false) << "Locking shard " << shard
                      << " for collection properties update returned nullptr";
    return Result{
        TRI_ERROR_INTERNAL,
        fmt::format(
            "Locking shard {} for collection properties update returned "
            "nullptr",
            shard)};
  }
  LOG_CTX("5a02d", DEBUG, loggerContext)
      << "Locked shard " << shard
      << " for collection properties update using transaction "
      << trxLock.get()->tid();

  // Replicate and wait for commit
  auto op = ReplicatedOperation::ModifyShard{shard, std::move(collectionId),
                                             std::move(properties)};
  auto fut = _guardedData.doUnderLock([&](auto& data) {
    if (data.didResign()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
          "Leader resigned before replicating ModifyShard operation");
    }

    return replicateOperation(
        op, ReplicationOptions{.waitForCommit = true, .waitForSync = true});
  });

  // Apply locally
  return std::move(fut).thenValue([self = shared_from_this(), shard = shard,
                                   op = std::move(op),
                                   trxLock = std::move(trxLock.get())](
                                      auto&& result) mutable {
    if (result.fail()) {
      return result.result();
    }
    auto logIndex = result.get();

    return self->_guardedData.doUnderLock(
        [logIndex, self, op = std::move(op), shard = shard,
         trxLock = std::move(trxLock)](auto& data) mutable -> Result {
          if (data.didResign()) {
            return TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED;
          }

          auto&& applyEntryRes =
              data.transactionHandler->applyEntry(std::move(op));

          // Once the operation is applied locally, we can release the shard
          // lock
          trxLock.reset();
          LOG_CTX("473fc", DEBUG, self->loggerContext)
              << "Released shard lock " << shard
              << " after collection properties update";

          if (applyEntryRes.fail()) {
            if (applyEntryRes.is(TRI_ERROR_SHUTTING_DOWN)) {
              LOG_CTX("34bab", DEBUG, self->loggerContext)
                  << "ModifyShard operation failed on the leader, due to the "
                     "server shutting down. ModifyShard log entry will not be "
                     "released.";
              return TRI_ERROR_NO_ERROR;
            }

            LOG_CTX("b5e46", FATAL, self->loggerContext)
                << "ModifyShard operation failed on the leader, after being "
                   "replicated to followers: "
                << applyEntryRes;
            TRI_ASSERT(false) << applyEntryRes;
            FATAL_ERROR_EXIT();
          }

          if (auto releaseRes = self->release(logIndex); releaseRes.fail()) {
            LOG_CTX("bbe40", ERR, self->loggerContext)
                << "Failed to call release on index " << logIndex << ": "
                << releaseRes;
          }
          return {};
        });
  });
}

auto DocumentLeaderState::dropShard(ShardID shard) -> futures::Future<Result> {
  auto op = ReplicatedOperation::DropShard{shard};

  auto fut = _guardedData.doUnderLock([&](auto& data) {
    if (data.didResign()) {
      THROW_ARANGO_EXCEPTION(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED);
    }

    return replicateOperation(
        op, ReplicationOptions{.waitForCommit = true, .waitForSync = true});
  });

  return std::move(fut).thenValue([self = shared_from_this(), shard = shard,
                                   op = std::move(op)](auto&& result) mutable {
    if (result.fail()) {
      return result.result();
    }
    auto logIndex = result.get();

    return self->_guardedData.doUnderLock([self, logIndex, op = std::move(op),
                                           shard =
                                               shard](auto& data) -> Result {
      if (data.didResign()) {
        return TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED;
      }

      // This will release the shard lock. Currently ongoing snapshot
      // transfers will not suffer from it, they will simply stop receiving
      // batches for this shard. Later, when they start applying entries, it
      // is going to be dropped anyway.
      self->_snapshotHandler.getLockedGuard().get()->giveUpOnShard(shard);

      // Note that any active transactions will be aborted automatically by
      // `TRI_vocbase_t::dropCollection`. This causes the leader to
      // replicate abort operations and release the log indexes associated
      // with these transactions.
      auto&& localDropShard = data.transactionHandler->applyEntry(op);

      // Some errors can be safely ignored, even though the operation has
      // been already replicated.
      auto errorHandlerResult =
          self->_errorHandler->handleOpResult(op, localDropShard);
      if (errorHandlerResult.fail()) {
        if (errorHandlerResult.is(TRI_ERROR_SHUTTING_DOWN)) {
          LOG_CTX("732c0", DEBUG, self->loggerContext)
              << "DropShard operation failed on the leader, due to the server "
                 "shutting down. DropShard log entry will not be released.";
          return TRI_ERROR_NO_ERROR;
        }

        LOG_CTX("6865f", FATAL, self->loggerContext)
            << "DropShard operation failed on the leader, after being "
               "replicated to followers: "
            << localDropShard;
        TRI_ASSERT(false) << localDropShard;
        FATAL_ERROR_EXIT();
      }

      if (auto releaseRes = self->release(logIndex); releaseRes.fail()) {
        LOG_CTX("6856b", ERR, self->loggerContext)
            << "Failed to call release on index " << logIndex << ": "
            << releaseRes;
      }
      return localDropShard;
    });
  });
}

// TODO
//      - The future returned by RocksDBCollection::createIndex should also be
//        propagated to here, so we don't block the thread.
auto DocumentLeaderState::createIndex(
    ShardID shard, VPackSlice indexInfo,
    std::shared_ptr<methods::Indexes::ProgressTracker> progress)
    -> futures::Future<Result> {
  // Index creation happens roughly as follows, most of it currently in
  // RocksDBCollection::createIndex:
  // 1) some work in the background, that we can ignore
  // 2) acquire an exclusive collection lock
  // 3) finish index creation
  // 4) replicate the CreateIndex operation, wait for raft-commit
  //    (the callback below)
  // 5) persist and publish the index
  auto releaseIndex = std::optional<LogIndex>();

  auto localIndexCreation = _guardedData.doUnderLock(
      [self = shared_from_this(), shard, indexInfo, &releaseIndex,
       progress = std::move(progress)](auto& data) mutable {
        if (data.didResign()) {
          return Result{TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
                        "Leader resigned prior to index creation"};
        }

        // Callback used to replicate the operation during
        // RocksDBCollection::createIndex
        auto callback = [self, shard, &releaseIndex](
                            velocypack::Slice indexDefinition) mutable
            -> futures::Future<Result> {
          auto op = ReplicatedOperation::CreateIndex{
              shard, velocypack::Builder(indexDefinition).sharedSlice()};
          auto replicationResult = co_await self->replicateOperation(
              std::move(op),
              replication2::replicated_state::document::ReplicationOptions{
                  .waitForCommit = true, .waitForSync = true});
          if (replicationResult.ok()) {
            auto const logIndex = replicationResult.get();
            releaseIndex = logIndex;
            // Increase the lowest safe index for replay: This must happen
            // *after* the create index operation has been raft-committed, but
            // *before* the index is persisted (i.e. before _inprogress=true is
            // removed).
            auto lowestSafeIndexesForReplayGuard =
                self->_lowestSafeIndexesForReplay.getLockedGuard();
            increaseAndPersistLowestSafeIndexForReplayTo(
                self->loggerContext, lowestSafeIndexesForReplayGuard.get(),
                *self->getStream(), shard, logIndex);
          }
          co_return std::move(replicationResult).result();
        };

        return self->_shardHandler->ensureIndex(
            shard, indexInfo, std::move(progress), std::move(callback));
      });
  auto indexId = helpers::extractId(indexInfo);
  TRI_ASSERT(indexId != IndexId::none() or localIndexCreation.fail());
  if (localIndexCreation.fail()) {
    return localIndexCreation;
  }

  // If the index creation succeeded, we must have saved a release index
  TRI_ASSERT(releaseIndex.has_value());
  if (releaseIndex.has_value()) {
    if (auto releaseRes = release(*releaseIndex); releaseRes.fail()) {
      LOG_CTX("3deea", ERR, loggerContext)
          << "Failed to call release on index " << *releaseIndex << ": "
          << releaseRes;
    }
  }

  return Result{};
}

auto DocumentLeaderState::dropIndex(ShardID shard, IndexId indexId)
    -> futures::Future<Result> {
  auto res = _shardHandler->lookupShard(shard);
  if (!res.ok()) {
    co_return std::move(res).result();
  }
  auto&& col = res.get();

  auto op = ReplicatedOperation::DropIndex{shard, indexId};

  auto trx = methods::Indexes::createTrxForDrop(*col);

  // we do not need to hold the _guardedData lock here - replicateOperation will
  // simply throw an exception if we have already resigned.

  // acquire an exclusive collection lock, so the DropIndex operation happens
  // in the log while no transaction is open
  auto beginRes = co_await trx->beginAsync();

  // replicate the DropIndex operation, but don't wait for it to be committed
  // yet
  auto replication = co_await replicateOperation(
      op, ReplicationOptions{.waitForCommit = false, .waitForSync = true});

  if (replication.fail()) {
    LOG_CTX("7c8bb", DEBUG, loggerContext)
        << "DropIndex operation failed to be replicated, will not be "
           "applied locally on the leader: "
        << replication.result();
    co_return replication.result();
  }
  // release the lock
  trx.reset();

  auto logIndex = replication.get();
  // wait for commit
  co_await getStream()->waitFor(logIndex);

  // Now drop the index.
  co_return _guardedData.doUnderLock([&](auto& data) mutable -> Result {
    if (data.didResign()) {
      return TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED;
    }

    auto localIndexRemoval = data.transactionHandler->applyEntry(op);

    // Some errors can be safely ignored, even though the operation
    // has been already replicated.
    auto errorHandlerResult =
        _errorHandler->handleOpResult(op, localIndexRemoval);
    if (errorHandlerResult.fail()) {
      if (errorHandlerResult.is(TRI_ERROR_SHUTTING_DOWN)) {
        LOG_CTX("4eb88", DEBUG, loggerContext)
            << "DropIndex operation failed on the leader, due to the "
               "server shutting down. DropIndex log entry will not be "
               "released.";
        return TRI_ERROR_NO_ERROR;
      }

      LOG_CTX("5c321", FATAL, loggerContext)
          << "DropIndex operation failed on the leader, after being "
             "replicated to followers: "
          << localIndexRemoval;
      TRI_ASSERT(false) << localIndexRemoval;
      FATAL_ERROR_EXIT();
    }

    if (auto releaseRes = release(logIndex); releaseRes.fail()) {
      LOG_CTX("57877", ERR, loggerContext) << "Failed to call release on index "
                                           << logIndex << ": " << releaseRes;
    }

    return localIndexRemoval;
  });
}

}  // namespace arangodb::replication2::replicated_state::document

#include "Replication2/ReplicatedState/ReplicatedStateImpl.tpp"

namespace arangodb::replication2::replicated_state {
template struct IReplicatedLeaderState<document::DocumentState>;
}
