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

#include "Replication2/StateMachines/Document/DocumentFollowerState.h"

#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedState/StateInterfaces.tpp"
#include "Replication2/StateMachines/Document/Actor/Scheduler.h"
#include "Replication2/StateMachines/Document/Actor/ApplyEntries.h"
#include "Replication2/StateMachines/Document/DocumentLeaderState.h"
#include "Replication2/StateMachines/Document/DocumentLogEntry.h"
#include "Replication2/StateMachines/Document/DocumentStateErrorHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateHandlersFactory.h"
#include "Replication2/StateMachines/Document/DocumentStateNetworkHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateShardHandler.h"
#include "Replication2/Streams/IMetadataTransaction.h"
#include "Scheduler/SchedulerFeature.h"
#include "VocBase/LogicalCollection.h"

#include "Actor/LocalRuntime.h"
#include <Basics/application-exit.h>
#include <Basics/Exceptions.h>
#include <Futures/Future.h>
#include <Logger/LogContextKeys.h>

#include <chrono>
#include <memory>
#include <thread>

namespace arangodb::replication2::replicated_state::document {

DocumentFollowerState::GuardedData::GuardedData(
    std::unique_ptr<DocumentCore> core, LoggerContext const& loggerContext)
    : loggerContext(loggerContext),
      core(std::move(core)),
      currentSnapshotVersion{0} {}

DocumentFollowerState::DocumentFollowerState(
    std::unique_ptr<DocumentCore> core, std::shared_ptr<Stream> stream,
    std::shared_ptr<IDocumentStateHandlersFactory> const& handlersFactory,
    std::shared_ptr<IScheduler> scheduler)
    : IReplicatedFollowerState(std::move(stream)),
      gid(core->gid),
      loggerContext(handlersFactory->createLogger(core->gid)
                        .with<logContextKeyStateComponent>("FollowerState")),
      _networkHandler(handlersFactory->createNetworkHandler(core->gid)),
      _shardHandler(
          handlersFactory->createShardHandler(core->getVocbase(), gid)),
      _transactionHandler(handlersFactory->createTransactionHandler(
          core->getVocbase(), gid, _shardHandler)),
      _errorHandler(handlersFactory->createErrorHandler(gid)),
      _guardedData(std::move(core), loggerContext),
      _runtime(std::make_shared<actor::LocalRuntime>(
          "FollowerState-" + to_string(gid),
          std::make_shared<actor::Scheduler>(std::move(scheduler)))),
      _lowestSafeIndexesForReplay(getStream()->getCommittedMetadata()) {
  // Get ready to replay the log
  _shardHandler->prepareShardsForLogReplay();
  _applyEntriesActor = _runtime->template spawn<actor::ApplyEntriesActor>(
      std::make_unique<actor::ApplyEntriesState>(loggerContext, *this));
  LOG_CTX("de019", INFO, loggerContext)
      << "Spawned ApplyEntries actor " << _applyEntriesActor.id;
}

DocumentFollowerState::~DocumentFollowerState() { shutdownRuntime(); }

void DocumentFollowerState::shutdownRuntime() noexcept {
  LOG_CTX("19dec", DEBUG, loggerContext) << "shutting down actor runtime";
  _runtime->shutdown();
  LOG_CTX("7289f", DEBUG, loggerContext) << "all actors finished";
}

auto DocumentFollowerState::resign() && noexcept
    -> std::unique_ptr<DocumentCore> {
  LOG_CTX("7289e", DEBUG, loggerContext) << "resigning follower state";
  _resigning.store(true);

  return _guardedData.doUnderLock([&](auto& data) {
    // we have to shutdown the runtime inside the lock to serialize this with a
    // concurrent applyEntries call (see the comment there for more details)
    // TODO - find a better way to wait for all the actors to finish
    _runtime->dispatch<actor::message::ApplyEntriesMessages>(
        _applyEntriesActor, _applyEntriesActor, actor::message::Resign{});

    shutdownRuntime();

    ADB_PROD_ASSERT(!data.didResign())
        << "Follower " << gid << " already resigned!";

    auto abortAllRes = _transactionHandler->applyEntry(
        ReplicatedOperation::AbortAllOngoingTrx{});
    ADB_PROD_ASSERT(abortAllRes.ok())
        << "Failed to abort ongoing transactions while resigning follower "
        << gid << ": " << abortAllRes;

    LOG_CTX("ed901", DEBUG, loggerContext)
        << "All ongoing transactions were aborted, as follower resigned";

    return std::move(data.core);
  });
}

auto DocumentFollowerState::getAssociatedShardList() const
    -> std::vector<ShardID> {
  auto collections = _shardHandler->getAvailableShards();

  std::vector<ShardID> shardIds;
  shardIds.reserve(collections.size());
  for (auto const& shard : collections) {
    auto maybeShardId = ShardID::shardIdFromString(shard->name());
    ADB_PROD_ASSERT(maybeShardId.ok())
        << "Tried to produce shard list on Database Server for a collection "
           "that is not a shard "
        << shard->name();
    shardIds.emplace_back(maybeShardId.get());
  }
  return shardIds;
}

auto DocumentFollowerState::acquireSnapshot(
    ParticipantId const& destination) noexcept -> futures::Future<Result> {
  LOG_CTX("1f67d", INFO, loggerContext)
      << "Trying to acquire snapshot from destination " << destination;

  auto snapshotVersion = _guardedData.doUnderLock(
      [self =
           shared_from_this()](auto& data) noexcept -> ResultT<std::uint64_t> {
        if (data.didResign()) {
          return Result{TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED};
        }

        if (auto abortAllRes = self->_transactionHandler->applyEntry(
                ReplicatedOperation::AbortAllOngoingTrx{});
            abortAllRes.fail()) {
          LOG_CTX("c863a", ERR, self->loggerContext)
              << "Failed to abort ongoing transactions before acquiring "
                 "snapshot: "
              << abortAllRes;
          return abortAllRes;
        }
        LOG_CTX("529bb", DEBUG, self->loggerContext)
            << "All ongoing transactions aborted before acquiring snapshot";

        if (auto dropAllRes = self->_shardHandler->dropAllShards();
            dropAllRes.fail()) {
          LOG_CTX("ae182", ERR, self->loggerContext)
              << "Failed to drop shards before acquiring snapshot: "
              << dropAllRes;
          return dropAllRes;
        }

        return ++data.currentSnapshotVersion;
      });

  if (snapshotVersion.fail()) {
    LOG_CTX("5ef29", DEBUG, loggerContext)
        << "Aborting snapshot transfer before contacting destination "
        << destination << ": " << snapshotVersion.result();
    return snapshotVersion.result();
  }

  // A follower may request a snapshot before leadership has been
  // established. A retry will occur in that case.
  auto leader = _networkHandler->getLeaderInterface(destination);
  auto snapshotStartRes =
      basics::catchToResultT([&leader]() { return leader->startSnapshot(); });
  if (snapshotStartRes.fail()) {
    LOG_CTX("954e3", DEBUG, loggerContext)
        << "Failed to start snapshot transfer with destination " << destination
        << ": " << snapshotStartRes.result();
    return snapshotStartRes.result();
  }

  return handleSnapshotTransfer(std::nullopt, leader, snapshotVersion.get(),
                                std::move(snapshotStartRes.get()))
      .then([leader, destination, self = shared_from_this()](
                auto&& tryResult) -> futures::Future<Result> {
        auto snapshotTransferResult =
            basics::catchToResultT([&] { return tryResult.get(); });
        if (snapshotTransferResult.fail()) {
          LOG_CTX("0c6d9", ERR, self->loggerContext)
              << "Snapshot transfer failed: "
              << snapshotTransferResult.result();
          return snapshotTransferResult.result();
        }

        if (!snapshotTransferResult->snapshotId.has_value()) {
          TRI_ASSERT(snapshotTransferResult->res.fail()) << self->gid;
          LOG_CTX("85628", ERR, self->loggerContext)
              << "Snapshot transfer failed: " << snapshotTransferResult->res;
          return snapshotTransferResult->res;
        }

        LOG_CTX("b4fcb", DEBUG, self->loggerContext)
            << "Snapshot " << *snapshotTransferResult->snapshotId
            << " data transfer over, will send finish request: "
            << snapshotTransferResult->res;

        auto snapshotFinishRes = basics::catchToResultT(
            [&leader, snapshotId = *snapshotTransferResult->snapshotId]() {
              return leader->finishSnapshot(snapshotId);
            });
        if (snapshotFinishRes.fail()) {
          LOG_CTX("4404d", ERR, self->loggerContext)
              << "Failed to initiate snapshot finishing procedure with "
                 "destination "
              << destination << ": " << snapshotFinishRes.result();
          return snapshotFinishRes.result();
        }

        return std::move(snapshotFinishRes.get())
            .then([snapshotTransferResult =
                       std::move(snapshotTransferResult).get()](auto&& tryRes) {
              auto res = basics::catchToResult([&] { return tryRes.get(); });
              if (res.fail()) {
                LOG_TOPIC("0e168", ERR, Logger::REPLICATION2)
                    << "Failed to finish snapshot "
                    << *snapshotTransferResult.snapshotId << ": " << res;
              }

              LOG_TOPIC("42ffd", DEBUG, Logger::REPLICATION2)
                  << "Successfully sent finish command for snapshot  "
                  << *snapshotTransferResult.snapshotId;

              TRI_ASSERT(snapshotTransferResult.res.fail() ||
                         (snapshotTransferResult.res.ok() &&
                          !snapshotTransferResult.reportFailure))
                  << snapshotTransferResult.res << " "
                  << snapshotTransferResult.reportFailure;

              if (snapshotTransferResult.reportFailure) {
                // Some failures don't need to be reported. For example, it's
                // totally fine for the follower to interrupt a snapshot
                // transfer while resigning, because there's no point in
                // continuing it.
                LOG_TOPIC("2883c", WARN, Logger::REPLICATION2)
                    << "During the processing of snapshot "
                    << *snapshotTransferResult.snapshotId
                    << ", the following problem occurred on the follower: "
                    << snapshotTransferResult.res;
                return snapshotTransferResult.res;
              }

              LOG_TOPIC("d73cb", DEBUG, Logger::REPLICATION2)
                  << "Snapshot " << *snapshotTransferResult.snapshotId
                  << " finished: " << snapshotTransferResult.res;
              return Result{};
            })
            .then([self](auto&& tryRes) {
              auto res = basics::catchToResult([&] { return tryRes.get(); });
              if (res.ok()) {
                // If we replayed a snapshot, we need to wait for views to
                // settle before we can continue. Otherwise, we would get into
                // issues with duplicate document ids
                self->_shardHandler->prepareShardsForLogReplay();
              }
              return res;
            });
      });
}

auto DocumentFollowerState::handleSnapshotTransfer(
    std::optional<SnapshotId> snapshotId,
    std::shared_ptr<IDocumentStateLeaderInterface> leader,
    std::uint64_t snapshotVersion,
    futures::Future<ResultT<SnapshotBatch>>&& snapshotFuture) noexcept
    -> futures::Future<SnapshotTransferResult> {
  return std::move(snapshotFuture)
      .then([weak = weak_from_this(), leader = std::move(leader), snapshotId,
             snapshotVersion](
                futures::Try<ResultT<SnapshotBatch>>&& tryResult) mutable
            -> futures::Future<SnapshotTransferResult> {
        auto catchRes =
            basics::catchToResultT([&] { return std::move(tryResult).get(); });
        if (catchRes.fail()) {
          return SnapshotTransferResult{.res = catchRes.result(),
                                        .reportFailure = true,
                                        .snapshotId = snapshotId};
        }

        auto snapshotRes = catchRes.get();
        if (snapshotRes.fail()) {
          return SnapshotTransferResult{.res = snapshotRes.result(),
                                        .reportFailure = true,
                                        .snapshotId = snapshotId};
        }

        if (snapshotId.has_value()) {
          if (snapshotId != snapshotRes->snapshotId) {
            auto err = fmt::format("Expected snapshot id {} but got {}",
                                   *snapshotId, snapshotRes->snapshotId);
            TRI_ASSERT(snapshotId == snapshotRes->snapshotId) << err;
            return SnapshotTransferResult{
                .res = {TRI_ERROR_INTERNAL, err},
                .reportFailure = true,
                .snapshotId = snapshotRes->snapshotId};
          }
        } else {
          // First batch of this snapshot, we got the ID now.
          snapshotId = snapshotRes->snapshotId;
        }

        auto self = weak.lock();
        if (self == nullptr) {
          // The follower resigned, there is no need to continue.
          return SnapshotTransferResult{
              .res = TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED,
              .reportFailure = true,
              .snapshotId = snapshotId};
        }

        // Apply operations locally
        bool reportingFailure = false;
        auto applyOperationsRes = self->_guardedData.doUnderLock(
            [&self, &snapshotRes, &reportingFailure,
             snapshotVersion](auto& data) -> Result {
              if (data.didResign() || self->_resigning) {
                reportingFailure = true;
                return TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED;
              }

              // The user may remove and add the server again. The leader
              // might do a compaction which the follower won't notice.
              // Hence, a new snapshot is required. This can happen so
              // quick, that one snapshot transfer is not yet completed
              // before another one is requested. Before populating the
              // shard, we have to make sure there's no new snapshot
              // transfer in progress.
              if (data.currentSnapshotVersion != snapshotVersion) {
                return {TRI_ERROR_INTERNAL,
                        "Snapshot transfer cancelled because a new one was "
                        "started!"};
              }

              LOG_CTX("c1d58", DEBUG, self->loggerContext)
                  << "Trying to apply " << snapshotRes->operations.size()
                  << " operations during snapshot transfer "
                  << snapshotRes->snapshotId;
              LOG_CTX("fcc92", TRACE, self->loggerContext)
                  << snapshotRes->snapshotId
                  << " operations: " << snapshotRes->operations;

              for (auto const& oper : snapshotRes->operations) {
                if (auto applyRes = std::visit(
                        overload{
                            [&](ReplicatedOperation::CreateIndex const& op)
                                -> Result {
                              // TODO snapshot transfer must include a set of
                              // lowest safe indexes; otherwise index creation
                              // during snapshot transfer might not be safe
                              LOG_DEVEL
                                  << "Creating index during snapshot transfer: "
                                     "this is currently unsafe!";
                              return self->_shardHandler->ensureIndex(
                                  op.shard, op.properties.slice(), nullptr,
                                  nullptr);
                            },
                            [&](auto const& op) -> Result {
                              return self->_transactionHandler->applyEntry(op);
                            },
                        },
                        oper.operation);
                    applyRes.fail()) {
                  reportingFailure = true;
                  return applyRes;
                }
              }

              return {};
            });
        if (applyOperationsRes.fail()) {
          return SnapshotTransferResult{.res = applyOperationsRes,
                                        .reportFailure = reportingFailure,
                                        .snapshotId = snapshotId};
        }

        // If there are more batches to come, fetch the next one
        if (snapshotRes->hasMore) {
          auto nextBatchRes = basics::catchToResultT([&leader, snapshotId]() {
            return leader->nextSnapshotBatch(*snapshotId);
          });
          if (nextBatchRes.fail()) {
            LOG_CTX("a732f", ERR, self->loggerContext)
                << "Failed to fetch the next batch of snapshot: "
                << *snapshotId;
            return SnapshotTransferResult{.res = nextBatchRes.result(),
                                          .reportFailure = true,
                                          .snapshotId = snapshotId};
          }
          return self->handleSnapshotTransfer(snapshotId, std::move(leader),
                                              snapshotVersion,
                                              std::move(nextBatchRes.get()));
        }

        // Snapshot transfer completed
        LOG_CTX("742df", DEBUG, self->loggerContext)
            << "Leader informed the follower there is no more data to be sent "
               "for snapshot "
            << snapshotRes->snapshotId;
        return SnapshotTransferResult{.snapshotId = snapshotId};
      });
}

auto DocumentFollowerState::applyEntries(
    std::unique_ptr<EntryIterator> ptr) noexcept -> futures::Future<Result> {
  return _guardedData.doUnderLock([&](auto& data) -> futures::Future<Result> {
    // We do not actually use data in here, but we use the guardedData lock to
    // serialize this code with a concurrent resign call.
    // We must avoid sending the ApplyEntries message if the actor has already
    // been finished. Therefore we need to check the _resigning flag _inside_
    // the lock. Likewise, resign must shutdown the runtime inside the lock,
    // _after_ it has set the resigning flag.
    if (_resigning.load()) {
      return Result{TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED};
    }

    futures::Promise<ResultT<std::optional<LogIndex>>> promise;
    auto future = promise.getFuture();
    _runtime->dispatch<actor::message::ApplyEntriesMessages>(
        _applyEntriesActor, _applyEntriesActor,
        actor::message::ApplyEntries{.entries = std::move(ptr),
                                     .promise = std::move(promise)});

    return std::move(future).thenValue(
        [me =
             weak_from_this()](ResultT<std::optional<LogIndex>> res) -> Result {
          if (res.fail()) {
            return res.result();
          }

          auto self = me.lock();
          if (self == nullptr) {
            return {TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED};
          }
          auto index = res.get();
          if (index.has_value()) {
            auto const& stream = self->getStream();
            // The follower might have resigned, so we need to be careful when
            // releasing an index on the stream
            auto releaseRes = basics::catchVoidToResult(
                [&] { stream->release(index.value()); });
            if (releaseRes.fail()) {
              LOG_CTX("10f07", ERR, self->loggerContext)
                  << "Failed to release index " << releaseRes;
            }
          }
          return Result{};
        });
  });
}

}  // namespace arangodb::replication2::replicated_state::document

#include "Replication2/ReplicatedState/ReplicatedStateImpl.tpp"

namespace arangodb::replication2::replicated_state {
template struct IReplicatedFollowerState<document::DocumentState>;
}
