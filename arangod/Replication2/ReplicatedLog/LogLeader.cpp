////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "LogLeader.h"

#include <Basics/Exceptions.h>
#include <Basics/Guarded.h>
#include <Basics/StringUtils.h>
#include <Basics/application-exit.h>
#include <Basics/debugging.h>
#include <Basics/system-compiler.h>
#include <Basics/voc-errors.h>
#include <Containers/ImmerMemoryPolicy.h>
#include <Futures/Future.h>
#include <Futures/Try.h>
#include <Logger/LogMacros.h>
#include <Logger/Logger.h>
#include <Logger/LoggerStream.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "Basics/ErrorCode.h"
#include "Futures/Promise.h"
#include "Futures/Unit.h"
#include "Logger/LogContextKeys.h"
#include "Metrics/Counter.h"
#include "Metrics/Gauge.h"
#include "Metrics/Histogram.h"
#include "Metrics/LogScale.h"
#include "Replication2/DeferredExecution.h"
#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/MetricsHelper.h"
#include "Replication2/ReplicatedLog/Algorithms.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/PersistedLog.h"
#include "Replication2/ReplicatedLog/ReplicatedLogIterator.h"
#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Replication2/IScheduler.h"
#include "Replication2/ReplicatedLog/Components/StorageManager.h"
#include "Replication2/ReplicatedLog/Components/CompactionManager.h"

#if (_MSC_VER >= 1)
// suppress warnings:
#pragma warning(push)
// conversion from 'size_t' to 'immer::detail::rbts::count_t', possible loss of
// data
#pragma warning(disable : 4267)
// result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift
// intended?)
#pragma warning(disable : 4334)
#endif
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

using namespace arangodb;
using namespace arangodb::replication2;

replicated_log::LogLeader::LogLeader(
    LoggerContext logContext, std::shared_ptr<ReplicatedLogMetrics> logMetrics,
    std::shared_ptr<ReplicatedLogGlobalSettings const> options,
    ParticipantId id, LogTerm term, LogIndex firstIndex,
    std::unique_ptr<IReplicatedStateHandle> stateHandle,
    std::shared_ptr<IAbstractFollowerFactory> followerFactory,
    std::shared_ptr<IScheduler> scheduler)
    : _logContext(std::move(logContext)),
      _logMetrics(std::move(logMetrics)),
      _options(std::move(options)),
      _followerFactory(std::move(followerFactory)),
      _scheduler(std::move(scheduler)),
      _id(std::move(id)),
      _currentTerm(term),
      _firstIndexOfCurrentTerm(firstIndex),
      _guardedLeaderData(*this, std::move(stateHandle), firstIndex) {
  _logMetrics->replicatedLogLeaderNumber->fetch_add(1);
}

replicated_log::LogLeader::~LogLeader() {
  _logMetrics->replicatedLogLeaderNumber->fetch_sub(1);
  if (auto queueEmpty =
          _guardedLeaderData.getLockedGuard()->_waitForQueue.empty();
      !queueEmpty) {
    TRI_ASSERT(false) << "expected wait-for-queue to be empty";
    LOG_CTX("ce7f1", ERR, _logContext) << "expected wait-for-queue to be empty";
  }
}

auto replicated_log::LogLeader::instantiateFollowers(
    LoggerContext const& logContext,
    std::shared_ptr<IAbstractFollowerFactory> followerFactory,
    std::shared_ptr<LocalFollower> const& localFollower,
    TermIndexPair lastEntry,
    std::shared_ptr<agency::ParticipantsConfig const> const& participantsConfig)
    -> std::unordered_map<ParticipantId, std::shared_ptr<FollowerInfo>> {
  std::unordered_map<ParticipantId, std::shared_ptr<FollowerInfo>>
      followers_map;
  followers_map.reserve(participantsConfig->participants.size() + 1);
  for (auto const& participant : participantsConfig->participants) {
    auto const& [it, inserted] = std::invoke([&]() {
      if (participant.first == localFollower->getParticipantId()) {
        return followers_map.emplace(
            participant.first, std::make_shared<FollowerInfo>(
                                   localFollower, lastEntry.index, logContext));
      } else {
        return followers_map.emplace(
            participant.first,
            std::make_shared<FollowerInfo>(
                followerFactory->constructFollower(participant.first),
                lastEntry.index, logContext));
      }
    });

    TRI_ASSERT(inserted) << "duplicate participant id: " << participant.first;
  }
  return followers_map;
}

namespace {
auto delayedFuture(IScheduler* sched,
                   std::chrono::steady_clock::duration duration)
    -> std::pair<IScheduler::WorkItemHandle, futures::Future<futures::Unit>> {
  if (sched) {
    auto p = futures::Promise<futures::Unit>();
    auto f = p.getFuture();
    auto item = sched->queueDelayed(
        "r2 appendentries", duration,
        [p = std::move(p)](bool cancelled) mutable {
          if (cancelled) {
            p.setException(basics::Exception(Result{TRI_ERROR_REQUEST_CANCELED},
                                             ADB_HERE));
          } else {
            p.setValue(futures::Unit{});
          }
        });

    return std::make_pair(std::move(item), std::move(f));
  }

  // std::this_thread::sleep_for(duration);
  return std::make_pair(nullptr, futures::Future<futures::Unit>{futures::unit});
}
}  // namespace

void replicated_log::LogLeader::handleResolvedPromiseSet(
    IScheduler* sched, ResolvedPromiseSet resolvedPromises,
    std::shared_ptr<ReplicatedLogMetrics> const& logMetrics) {
  for (auto& promise : resolvedPromises._set) {
    TRI_ASSERT(promise.second.valid());
    sched->queue([promise = std::move(promise.second),
                  result = resolvedPromises.result]() mutable {
      promise.setValue(result);
    });
  }
}

void replicated_log::LogLeader::executeAppendEntriesRequests(
    std::vector<std::optional<PreparedAppendEntryRequest>> requests,
    std::shared_ptr<ReplicatedLogMetrics> const& logMetrics,
    IScheduler* sched) {
  for (auto& req : requests) {
    if (req.has_value()) {
      auto [item, f] = delayedFuture(sched, req->_executionDelay);
      if (item != nullptr) {
        if (auto follower = req->_follower.lock(); !follower) {
          continue;  // follower was dropped
        } else {
          follower->lastRequestHandle = std::move(item);
        }
      }
      std::move(f).thenFinal([req = std::move(req),
                              logMetrics](auto&&) mutable {
        auto follower = req->_follower.lock();
        auto logLeader = req->_parentLog.lock();
        if (logLeader == nullptr || follower == nullptr) {
          LOG_TOPIC("de312", TRACE, Logger::REPLICATION2)
              << "parent log already gone, not sending any more "
                 "AppendEntryRequests";
          return;
        }

        auto [request, lastIndex] =
            logLeader->_guardedLeaderData.doUnderLock([&](auto const& self) {
              auto [releaseIndex, lowestIndexToKeep] =
                  logLeader->_compactionManager->getIndexes();
              auto lastAvailableIndex =
                  self._inMemoryLog.getLastTermIndexPair();
              LOG_CTX("71801", TRACE, follower->logContext)
                  << "last matched index = " << follower->nextPrevLogIndex
                  << ", current index = " << lastAvailableIndex
                  << ", last acked commit index = "
                  << follower->lastAckedCommitIndex
                  << ", current commit index = " << self._commitIndex
                  << ", last acked litk = "
                  << follower->lastAckedLowestIndexToKeep
                  << ", current litk = " << lowestIndexToKeep;
              // We can only get here if there is some new information
              // for this follower
              TRI_ASSERT(
                  follower->nextPrevLogIndex != lastAvailableIndex.index ||
                  self._commitIndex != follower->lastAckedCommitIndex ||
                  lowestIndexToKeep != follower->lastAckedLowestIndexToKeep);

              return self.createAppendEntriesRequest(*follower,
                                                     lastAvailableIndex);
            });

        auto messageId = request.messageId;
        LOG_CTX("1b0ec", TRACE, follower->logContext)
            << "sending append entries, messageId = " << messageId;

        // We take the start time here again to have a more precise
        // measurement. (And do not use follower._lastRequestStartTP)
        // TODO really needed?
        auto startTime = std::chrono::steady_clock::now();
        // Capture a weak pointer `parentLog` that will be locked
        // when the request returns. If the locking is successful
        // we are still in the same term.
        follower->_impl->appendEntries(std::move(request))
            .thenFinal([weakParentLog = req->_parentLog,
                        followerWeak = req->_follower, lastIndex = lastIndex,
                        currentCommitIndex = request.leaderCommit,
                        currentLITK = request.lowestIndexToKeep,
                        currentTerm = logLeader->_currentTerm,
                        messageId = messageId, startTime,
                        logMetrics = logMetrics](
                           futures::Try<AppendEntriesResult>&& res) noexcept {
              // This has to remain noexcept, because the code below is not
              // exception safe
              auto const endTime = std::chrono::steady_clock::now();

              auto self = weakParentLog.lock();
              auto follower = followerWeak.lock();
              if (self != nullptr && follower != nullptr) {
                using namespace std::chrono_literals;
                auto const duration = endTime - startTime;
                self->_logMetrics->replicatedLogAppendEntriesRttUs->count(
                    duration / 1us);
                LOG_CTX("8ff44", TRACE, follower->logContext)
                    << "received append entries response, messageId = "
                    << messageId;
                auto [preparedRequests, resolvedPromises] = std::invoke(
                    [&]() -> std::pair<std::vector<std::optional<
                                           PreparedAppendEntryRequest>>,
                                       ResolvedPromiseSet> {
                      auto guarded = self->acquireMutex();
                      if (!guarded->_didResign) {
                        return guarded->handleAppendEntriesResponse(
                            *follower, lastIndex, currentCommitIndex,
                            currentLITK, currentTerm, std::move(res),
                            endTime - startTime, messageId);
                      } else {
                        LOG_CTX("da116", DEBUG, follower->logContext)
                            << "received response from follower but leader "
                               "already resigned, messageId = "
                            << messageId;
                      }
                      return {};
                    });

                handleResolvedPromiseSet(self->_scheduler.get(),
                                         std::move(resolvedPromises),
                                         logMetrics);
                executeAppendEntriesRequests(std::move(preparedRequests),
                                             logMetrics,
                                             self->_scheduler.get());
              } else {
                if (follower == nullptr) {
                  LOG_TOPIC("6f490", DEBUG, Logger::REPLICATION2)
                      << "follower already gone.";
                } else {
                  LOG_CTX("de300", DEBUG, follower->logContext)
                      << "parent log already gone, messageId = " << messageId;
                }
              }
            });
      });
    }
  }
}

auto replicated_log::LogLeader::construct(
    std::unique_ptr<replicated_state::IStorageEngineMethods>&& methods,
    std::shared_ptr<agency::ParticipantsConfig const> participantsConfig,
    ParticipantId id, LogTerm term, LoggerContext const& logContext,
    std::shared_ptr<ReplicatedLogMetrics> logMetrics,
    std::shared_ptr<ReplicatedLogGlobalSettings const> options,
    std::unique_ptr<IReplicatedStateHandle> stateHandle,
    std::shared_ptr<IAbstractFollowerFactory> followerFactory,
    std::shared_ptr<IScheduler> scheduler) -> std::shared_ptr<LogLeader> {
  auto const& config = participantsConfig->config;
  auto const& participants = participantsConfig->participants;

  if (ADB_UNLIKELY(methods == nullptr)) {
    auto followerIds = std::vector<std::string>{};
    std::transform(
        participants.begin(), participants.end(),
        std::back_inserter(followerIds),
        [](auto const& follower) -> std::string { return follower.first; });
    auto message = basics::StringUtils::concatT(
        "LogCore missing when constructing LogLeader, leader id: ", id,
        "term: ", term, "effectiveWriteConcern: ", config.effectiveWriteConcern,
        "followers: ", basics::StringUtils::join(followerIds, ", "));
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, std::move(message));
  }

  // Workaround to be able to use make_shared, while LogLeader's constructor
  // is actually protected.
  struct MakeSharedLogLeader : LogLeader {
   public:
    MakeSharedLogLeader(
        LoggerContext logContext,
        std::shared_ptr<ReplicatedLogMetrics> logMetrics,
        std::shared_ptr<ReplicatedLogGlobalSettings const> options,
        ParticipantId id, LogTerm term, LogIndex firstIndexOfCurrentTerm,
        std::unique_ptr<IReplicatedStateHandle> stateHandle,
        std::shared_ptr<IAbstractFollowerFactory> followerFactory,
        std::shared_ptr<IScheduler> scheduler)
        : LogLeader(std::move(logContext), std::move(logMetrics),
                    std::move(options), std::move(id), term,
                    firstIndexOfCurrentTerm, std::move(stateHandle),
                    std::move(followerFactory), std::move(scheduler)) {}
  };

  {
    auto metaResult = methods->readMetadata();
    if (metaResult.fail()) {
      THROW_ARANGO_EXCEPTION(metaResult.result());
    }
    auto& meta = metaResult.get();
    meta.snapshot.status = replicated_state::SnapshotStatus::kCompleted;
    std::ignore = methods->updateMetadata(meta);
  }

  // Note that although we add an entry to establish our leadership
  // we do still want to use the unchanged lastIndex to initialize
  // our followers with, as none of them can possibly have this entry.
  // This is particularly important for the LocalFollower, which blindly
  // accepts appendEntriesRequests, and we would thus forget persisting this
  // entry on the leader!

  auto commonLogContext =
      logContext.with<logContextKeyTerm>(term).with<logContextKeyLeaderId>(id);

  auto storageManager = std::make_shared<StorageManager>(
      std::move(methods), commonLogContext.with<logContextKeyLogComponent>(
                              "local-storage-manager"));
  auto lastIndex =
      storageManager->getTermIndexMapping().getLastIndex().value_or(
          TermIndexPair{});

  // if this assertion triggers there is an entry present in the log
  // that has the current term. Did create a different leader with the same term
  // in your test?
  if (lastIndex.term >= term) {
    LOG_CTX("8ed2f", FATAL, logContext)
        << "Failed to construct log leader. Current term is " << term
        << " but spearhead is already at " << lastIndex.term;
    FATAL_ERROR_EXIT();  // This must never happen in production
  }

  auto leader = std::make_shared<MakeSharedLogLeader>(
      commonLogContext.with<logContextKeyLogComponent>("leader"),
      std::move(logMetrics), options, std::move(id), term, lastIndex.index + 1u,
      std::move(stateHandle), followerFactory, scheduler);

  auto compactionManager = std::make_shared<CompactionManager>(
      *storageManager, options,
      commonLogContext.with<logContextKeyLogComponent>(
          "local-compaction-manager"));
  auto localFollower = std::make_shared<LocalFollower>(
      *leader,
      commonLogContext.with<logContextKeyLogComponent>("local-follower"),
      storageManager, lastIndex);

  try {
    TRI_ASSERT(participantsConfig != nullptr);
    {
      auto leaderDataGuard = leader->acquireMutex();

      leaderDataGuard->_follower =
          instantiateFollowers(commonLogContext, followerFactory, localFollower,
                               lastIndex, participantsConfig);
      leaderDataGuard->activeParticipantsConfig = participantsConfig;
      leader->_localFollower = std::move(localFollower);
      leader->_storageManager = std::move(storageManager);
      leader->_compactionManager = std::move(compactionManager);
      TRI_ASSERT(leaderDataGuard->_follower.size() >=
                 config.effectiveWriteConcern)
          << "actual followers: " << leaderDataGuard->_follower.size()
          << " effectiveWriteConcern: " << config.effectiveWriteConcern;
      TRI_ASSERT(
          leaderDataGuard->_follower.size() ==
          leaderDataGuard->activeParticipantsConfig->participants.size());
      TRI_ASSERT(std::all_of(leaderDataGuard->_follower.begin(),
                             leaderDataGuard->_follower.end(),
                             [&](auto const& it) {
                               return leaderDataGuard->activeParticipantsConfig
                                   ->participants.contains(it.first);
                             }));
    }

    leader->establishLeadership(std::move(participantsConfig));
    leader->triggerAsyncReplication();
    return leader;
  } catch (...) {
    // In case of an exception, the `logCore` parameter *must* stay unchanged.
    ADB_PROD_ASSERT(methods == nullptr);
    methods = std::move(*storageManager).resign();
    ADB_PROD_ASSERT(methods != nullptr);
    throw;
  }
}

auto replicated_log::LogLeader::acquireMutex() -> LogLeader::Guard {
  return _guardedLeaderData.getLockedGuard();
}

auto replicated_log::LogLeader::acquireMutex() const -> LogLeader::ConstGuard {
  return _guardedLeaderData.getLockedGuard();
}

auto replicated_log::LogLeader::getStatus() const -> LogStatus {
  return _guardedLeaderData.doUnderLock([this, term = _currentTerm](
                                            GuardedLeaderData const&
                                                leaderData) {
    if (leaderData._didResign) {
      throw ParticipantResignedException(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
    }
    auto [releaseIndex, lowestIndexToKeep] = _compactionManager->getIndexes();
    LeaderStatus status;
    status.local = leaderData.getLocalStatistics();
    status.term = term;
    status.compactionStatus = _compactionManager->getCompactionStatus();
    status.lowestIndexToKeep = lowestIndexToKeep;
    status.firstInMemoryIndex = leaderData._inMemoryLog.getFirstIndex();
    status.lastCommitStatus = leaderData._lastCommitFailReason;
    status.leadershipEstablished = leaderData._leadershipEstablished;
    status.activeParticipantsConfig = *leaderData.activeParticipantsConfig;
    if (auto const config = leaderData.committedParticipantsConfig;
        config != nullptr) {
      status.committedParticipantsConfig = *config;
    }
    for (auto const& [pid, f] : leaderData._follower) {
      auto lastRequestLatencyMS =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              f->_lastRequestLatency);
      auto state = std::invoke([&, &f = f] {
        switch (f->_state) {
          case FollowerInfo::State::ERROR_BACKOFF:
            return FollowerState::withErrorBackoff(
                std::chrono::duration_cast<
                    std::chrono::duration<double, std::milli>>(
                    f->_errorBackoffEndTP - std::chrono::steady_clock::now()),
                f->numErrorsSinceLastAnswer);
          case FollowerInfo::State::REQUEST_IN_FLIGHT:
            return FollowerState::withRequestInFlight(
                std::chrono::duration_cast<
                    std::chrono::duration<double, std::milli>>(
                    std::chrono::steady_clock::now() - f->_lastRequestStartTP));
          default:
            return FollowerState::withUpToDate();
        }
      });
      auto const& participantId = f->_impl->getParticipantId();
      TRI_ASSERT(pid == participantId);
      TRI_ASSERT(!pid.empty());
      status.follower.emplace(
          participantId,
          FollowerStatistics{
              LogStatistics{f->lastAckedIndex, f->lastAckedCommitIndex},
              f->lastErrorReason, lastRequestLatencyMS, state,
              f->nextPrevLogIndex, f->snapshotAvailable});
    }

    status.commitLagMS = leaderData.calculateCommitLag();
    return LogStatus{std::move(status)};
  });
}

namespace {
auto getLocalState(replicated_state::Status const& status)
    -> replicated_log::LocalStateMachineStatus {
  using namespace replicated_state;
  using namespace replicated_log;
  return std::visit(
      overload{
          [](Status::Leader const& status) {
            return std::visit(
                overload{
                    [](Status::Leader::Resigned const&) {
                      return LocalStateMachineStatus::kUnconfigured;
                    },
                    [](Status::Leader::InRecovery const&) {
                      return LocalStateMachineStatus::kRecovery;
                    },
                    [](Status::Leader::Operational const&) {
                      return LocalStateMachineStatus::kOperational;
                    },
                },
                status.value);
          },
          [](Status::Follower const& status) {
            return LocalStateMachineStatus::kUnconfigured;
          },
          [](Status::Unconfigured const& status) {
            return LocalStateMachineStatus::kUnconfigured;
          },
      },
      status.value);
}
}  // namespace

auto replicated_log::LogLeader::getQuickStatus() const -> QuickLogStatus {
  // TODO This is a bug and requires a fix. The unique ptr of the stateHandle
  //  should not be dereference without having the _guardedLeaderData lock.
  //  resign could be called in the meantime.
  auto& stateHandle = *_guardedLeaderData.getLockedGuard()->_stateHandle;
  auto const localState = getLocalState(stateHandle.getInternalStatus());
  auto guard = _guardedLeaderData.getLockedGuard();
  if (guard->_didResign) {
    throw ParticipantResignedException(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
  }
  auto commitFailReason = std::optional<CommitFailReason>{};
  if (guard->calculateCommitLag() > std::chrono::seconds{20}) {
    commitFailReason = guard->_lastCommitFailReason;
  }
  std::vector<ParticipantId> followersWithSnapshot;
  followersWithSnapshot.reserve(guard->_follower.size());
  for (auto const& [pid, follower] : guard->_follower) {
    if (pid != _id && follower->snapshotAvailable) {
      followersWithSnapshot.emplace_back(pid);
    }
  }
  return QuickLogStatus{
      .role = ParticipantRole::kLeader,
      .localState = localState,
      .term = _currentTerm,
      .local = guard->getLocalStatistics(),
      .leadershipEstablished = guard->_leadershipEstablished,
      .snapshotAvailable = true,
      .commitFailReason = commitFailReason,
      .activeParticipantsConfig = guard->activeParticipantsConfig,
      .committedParticipantsConfig = guard->committedParticipantsConfig,
      .followersWithSnapshot = std::move(followersWithSnapshot)};
}

auto replicated_log::LogLeader::insert(LogPayload payload, bool waitForSync)
    -> LogIndex {
  auto index =
      insert(std::move(payload), waitForSync, doNotTriggerAsyncReplication);
  triggerAsyncReplication();
  return index;
}

auto replicated_log::LogLeader::insert(LogPayload payload, bool waitForSync,
                                       DoNotTriggerAsyncReplication)
    -> LogIndex {
  auto const insertTp = InMemoryLogEntry::clock::now();
  // Currently we use a mutex. Is this the only valid semantic?
  return _guardedLeaderData.doUnderLock([&](GuardedLeaderData& leaderData) {
    return leaderData.insertInternal(std::move(payload), waitForSync, insertTp);
  });
}

auto replicated_log::LogLeader::GuardedLeaderData::insertInternal(
    std::variant<LogMetaPayload, LogPayload> payload, bool waitForSync,
    std::optional<InMemoryLogEntry::clock::time_point> insertTp) -> LogIndex {
  // TODO for now only waitForSync=true is supported.
  waitForSync = true;  // by setting this to true, the waitForSync flag is set
                       // for all log entries, independent of the log config.
  if (this->_didResign) {
    throw ParticipantResignedException(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
  }
  auto const index = this->_inMemoryLog.getNextIndex();
  auto const payloadSize = std::holds_alternative<LogPayload>(payload)
                               ? std::get<LogPayload>(payload).byteSize()
                               : 0;
  bool const isMetaLogEntry = std::holds_alternative<LogMetaPayload>(payload);
  auto logEntry = InMemoryLogEntry(
      PersistingLogEntry(TermIndexPair{_self._currentTerm, index},
                         std::move(payload)),
      waitForSync);
  logEntry.setInsertTp(insertTp.has_value() ? *insertTp
                                            : InMemoryLogEntry::clock::now());
  auto size = logEntry.entry().approxByteSize();
  this->_inMemoryLog.appendInPlace(_self._logContext, std::move(logEntry));
  _self._logMetrics->replicatedLogInsertsBytes->count(payloadSize);
  _self._logMetrics->leaderNumInMemoryEntries->fetch_add(1);
  _self._logMetrics->leaderNumInMemoryBytes->fetch_add(size);
  if (isMetaLogEntry) {
    _self._logMetrics->replicatedLogNumberMetaEntries->count(1);
  } else {
    _self._logMetrics->replicatedLogNumberAcceptedEntries->count(1);
  }
  return index;
}

auto replicated_log::LogLeader::waitFor(LogIndex index) -> WaitForFuture {
  return _guardedLeaderData.doUnderLock([index](auto& leaderData) {
    if (leaderData._didResign) {
      auto promise = WaitForPromise{};
      promise.setException(ParticipantResignedException(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE));
      return promise.getFuture();
    }
    if (leaderData._commitIndex >= index) {
      return futures::Future<WaitForResult>{
          std::in_place, leaderData._commitIndex, leaderData._lastQuorum};
    }
    auto it = leaderData._waitForQueue.emplace(index, WaitForPromise{});
    auto& promise = it->second;
    auto&& future = promise.getFuture();
    TRI_ASSERT(future.valid());
    return std::move(future);
  });
}

auto replicated_log::LogLeader::getParticipantId() const noexcept
    -> ParticipantId const& {
  return _id;
}

auto replicated_log::LogLeader::triggerAsyncReplication() -> void {
  auto preparedRequests = _guardedLeaderData.doUnderLock([](auto& leaderData) {
    if (leaderData._didResign) {
      throw ParticipantResignedException(
          TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
    }
    return leaderData.prepareAppendEntries();
  });
  executeAppendEntriesRequests(std::move(preparedRequests), _logMetrics,
                               _scheduler.get());
}

auto replicated_log::LogLeader::GuardedLeaderData::updateCommitIndexLeader(
    LogIndex newIndex, std::shared_ptr<QuorumData> quorum)
    -> ResolvedPromiseSet {
  LOG_CTX("a9a7e", TRACE, _self._logContext)
      << "updating commit index to " << newIndex << " with quorum "
      << quorum->quorum;
  auto oldIndex = _commitIndex;

  TRI_ASSERT(_commitIndex < newIndex)
      << "_commitIndex == " << _commitIndex << ", newIndex == " << newIndex;
  _self._logMetrics->replicatedLogNumberCommittedEntries->count(
      newIndex.value - _commitIndex.value);
  _commitIndex = newIndex;
  _lastQuorum = quorum;

  // Update commit time metrics
  auto const commitTp = InMemoryLogEntry::clock::now();

  for (auto const& it : _inMemoryLog.slice(oldIndex, newIndex + 1)) {
    using namespace std::chrono_literals;
    auto const entryDuration = commitTp - it.insertTp();
    _self._logMetrics->replicatedLogInsertsRtt->count(entryDuration / 1us);
  }

  // potentially evict parts of the inMemoryLog.
  auto maxDiskIndex =
      _self._storageManager->getTermIndexMapping().getLastIndex().value_or(
          TermIndexPair{});
  auto evictStopIndex = std::min(_commitIndex, maxDiskIndex.index);
  std::size_t releasedMemory = 0, numEntriesEvicted = 0;
  for (auto const& memtry : _inMemoryLog.slice(LogIndex{0}, evictStopIndex)) {
    releasedMemory += memtry.entry().approxByteSize();
    numEntriesEvicted += 1;
  }
  // remove upto commit index, but keep non-locally persisted log
  _inMemoryLog = _inMemoryLog.removeFront(evictStopIndex);
  _self._logMetrics->leaderNumInMemoryEntries->fetch_sub(numEntriesEvicted);
  _self._logMetrics->leaderNumInMemoryBytes->fetch_sub(releasedMemory);

  struct MethodsImpl : IReplicatedLogLeaderMethods {
    explicit MethodsImpl(LogLeader& log) : _log(log) {}
    auto releaseIndex(LogIndex index) -> void override {
      if (auto res = _log.release(index); res.fail()) {
        THROW_ARANGO_EXCEPTION(res);
      }
    }
    auto getCommittedLogIterator(std::optional<LogRange> range)
        -> std::unique_ptr<LogRangeIterator> override {
      return _log.getLogConsumerIterator(range);
    }
    auto insert(LogPayload payload) -> LogIndex override {
      return _log.insert(std::move(payload));
    }
    auto insertDeferred(LogPayload payload)
        -> std::pair<LogIndex, DeferredAction> override {
      auto index =
          _log.insert(std::move(payload), false, doNotTriggerAsyncReplication);
      auto action = DeferredAction([weak = _log.weak_from_this()]() noexcept {
        if (auto self = weak.lock(); self != nullptr) {
          try {
            self->triggerAsyncReplication();
          } catch (ParticipantResignedException const&) {
            // The log resigned; this is fine, we can just ignore it.
          } catch (std::exception const& ex) {
            LOG_CTX("f96cd", INFO, self->_logContext)
                << "Unhandled exception in insertDeferred: " << ex.what();
          }
        }
      });
      return std::make_pair(index, std::move(action));
    }
    auto waitFor(LogIndex index) -> WaitForFuture override {
      return _log.waitFor(index);
    }
    auto waitForIterator(LogIndex index) -> WaitForIteratorFuture override {
      return _log.waitForIterator(index);
    }

    LogLeader& _log;
  };

  if (not _leadershipEstablished) {
    // leadership is established if commitIndex is non-zero
    ADB_PROD_ASSERT(newIndex > LogIndex{0});
    _leadershipEstablished = true;
    LOG_CTX("f1136", DEBUG, _self._logContext) << "leadership established";
    _stateHandle->leadershipEstablished(std::make_unique<MethodsImpl>(_self));
  }

  // Currently unused, and could deadlock with recoverEntries, because that in
  // return calls insert
  //_stateHandle->updateCommitIndex(_commitIndex);

  try {
    WaitForQueue toBeResolved;
    auto const end = _waitForQueue.upper_bound(_commitIndex);
    for (auto it = _waitForQueue.begin(); it != end;) {
      LOG_CTX("37f9d", TRACE, _self._logContext)
          << "resolving promise for index " << it->first;
      toBeResolved.insert(_waitForQueue.extract(it++));
    }
    return ResolvedPromiseSet{_commitIndex, std::move(toBeResolved),
                              WaitForResult(newIndex, std::move(quorum))};
  } catch (std::exception const& e) {
    // If those promises are not fulfilled we can not continue.
    // Note that the move constructor of std::multi_map is not noexcept.
    LOG_CTX("e7a4e", FATAL, _self._logContext)
        << "failed to fulfill replication promises due to exception; system "
           "can not continue. message: "
        << e.what();
    FATAL_ERROR_EXIT();
  } catch (...) {
    // If those promises are not fulfilled we can not continue.
    // Note that the move constructor of std::multi_map is not noexcept.
    LOG_CTX("c0bbb", FATAL, _self._logContext)
        << "failed to fulfill replication promises due to exception; system "
           "can not continue";
    FATAL_ERROR_EXIT();
  }
}

auto replicated_log::LogLeader::GuardedLeaderData::prepareAppendEntries()
    -> std::vector<std::optional<PreparedAppendEntryRequest>> {
  auto appendEntryRequests =
      std::vector<std::optional<PreparedAppendEntryRequest>>{};
  appendEntryRequests.reserve(_follower.size());
  std::transform(
      _follower.begin(), _follower.end(),
      std::back_inserter(appendEntryRequests),
      [this](auto& follower) { return prepareAppendEntry(follower.second); });
  return appendEntryRequests;
}

auto replicated_log::LogLeader::GuardedLeaderData::prepareAppendEntry(
    std::shared_ptr<FollowerInfo> follower)
    -> std::optional<PreparedAppendEntryRequest> {
  if (follower->_state != FollowerInfo::State::IDLE) {
    LOG_CTX("1d7b6", TRACE, follower->logContext)
        << "request in flight - skipping";
    return std::nullopt;  // wait for the request to return
  }

  auto [releaseIndex, lowestIndexToKeep] =
      _self._compactionManager->getIndexes();

  auto const lastAvailableIndex = _inMemoryLog.getLastTermIndexPair();
  LOG_CTX("8844a", TRACE, follower->logContext)
      << "last matched index = " << follower->nextPrevLogIndex
      << ", current index = " << lastAvailableIndex
      << ", last acked commit index = " << follower->lastAckedCommitIndex
      << ", current commit index = " << _commitIndex
      << ", last acked lci = " << follower->lastAckedLowestIndexToKeep
      << ", current lci = " << lowestIndexToKeep;
  if (follower->nextPrevLogIndex == lastAvailableIndex.index &&
      _commitIndex == follower->lastAckedCommitIndex &&
      lowestIndexToKeep == follower->lastAckedLowestIndexToKeep) {
    LOG_CTX("74b71", TRACE, follower->logContext) << "up to date";
    return std::nullopt;  // nothing to replicate
  }

  auto const executionDelay = std::invoke([&] {
    using namespace std::chrono_literals;
    if (follower->numErrorsSinceLastAnswer > 0) {
      // Capped exponential backoff. Wait for 100us, 200us, 400us, ...
      // until at most 100us * 2 ** 17 == 13.11s.
      auto executionDelay =
          100us *
          (1u << std::min(follower->numErrorsSinceLastAnswer, std::size_t{17}));
      LOG_CTX("2a6f7", DEBUG, follower->logContext)
          << follower->numErrorsSinceLastAnswer
          << " requests failed, last one was " << follower->lastSentMessageId
          << " - waiting " << executionDelay / 1ms
          << "ms before sending next message.";
      follower->_state = FollowerInfo::State::ERROR_BACKOFF;
      follower->_errorBackoffEndTP =
          std::chrono::steady_clock::now() + executionDelay;
      return executionDelay;
    } else {
      follower->_state = FollowerInfo::State::PREPARE;
      return 0us;
    }
  });

  return PreparedAppendEntryRequest{_self.shared_from_this(),
                                    std::move(follower), executionDelay};
}

auto replicated_log::LogLeader::GuardedLeaderData::createAppendEntriesRequest(
    replicated_log::LogLeader::FollowerInfo& follower,
    TermIndexPair const& lastAvailableIndex) const
    -> std::pair<AppendEntriesRequest, TermIndexPair> {
  auto prevLogTerm = [&]() -> std::optional<LogTerm> {
    if (_inMemoryLog.getIndexRange().contains(follower.nextPrevLogIndex)) {
      return _inMemoryLog.getEntryByIndex(follower.nextPrevLogIndex)
          ->entry()
          .logTerm();
    } else {
      return _self._storageManager->getTermIndexMapping().getTermOfIndex(
          follower.nextPrevLogIndex);
    }
  }();

  auto [releaseIndex, lowestIndexToKeep] =
      _self._compactionManager->getIndexes();
  AppendEntriesRequest req;
  req.leaderCommit = _commitIndex;
  req.lowestIndexToKeep = lowestIndexToKeep;
  req.leaderTerm = _self._currentTerm;
  req.leaderId = _self._id;
  req.waitForSync = this->activeParticipantsConfig->config.waitForSync;
  req.messageId = ++follower.lastSentMessageId;

  follower._state = FollowerInfo::State::REQUEST_IN_FLIGHT;
  follower._lastRequestStartTP = std::chrono::steady_clock::now();

  if (prevLogTerm) {
    req.prevLogEntry.index = follower.nextPrevLogIndex;
    req.prevLogEntry.term = *prevLogTerm;
    TRI_ASSERT(req.prevLogEntry.index == follower.nextPrevLogIndex);
  } else {
    req.prevLogEntry.index = LogIndex{0};
    req.prevLogEntry.term = LogTerm{0};
  }

  // Now get a iterator starting at follower.nextPrevLogIndex + 1 but also
  // including the InMemory part.

  if (_inMemoryLog.getLastIndex() > follower.nextPrevLogIndex) {
    auto it = getInternalLogIterator(follower.nextPrevLogIndex + 1);
    auto transientEntries = decltype(req.entries)::transient_type{};
    auto sizeCounter = std::size_t{0};
    while (auto entry = it->next()) {
      req.waitForSync |= entry->getWaitForSync();
      transientEntries.push_back(*entry);
      sizeCounter += entry->entry().approxByteSize();

      if (sizeCounter >= _self._options->_thresholdNetworkBatchSize) {
        break;
      }
    }
    req.entries = std::move(transientEntries).persistent();

    _self._logMetrics->replicatedLogAppendEntriesNumEntries->count(
        req.entries.size());
    _self._logMetrics->replicatedLogAppendEntriesSize->count(sizeCounter);
  }

  auto isEmptyAppendEntries = req.entries.empty();
  auto lastIndex = isEmptyAppendEntries
                       ? lastAvailableIndex
                       : req.entries.back().entry().logTermIndexPair();

  LOG_CTX("af3c6", TRACE, follower.logContext)
      << "creating append entries request with " << req.entries.size()
      << " entries , prevLogEntry.term = " << req.prevLogEntry.term
      << ", prevLogEntry.index = " << req.prevLogEntry.index
      << ", leaderCommit = " << req.leaderCommit
      << ", lci = " << req.lowestIndexToKeep << ", msg-id = " << req.messageId;

  return std::make_pair(std::move(req), lastIndex);
}

auto replicated_log::LogLeader::GuardedLeaderData::handleAppendEntriesResponse(
    FollowerInfo& follower, TermIndexPair lastIndex,
    LogIndex currentCommitIndex, LogIndex currentLITK, LogTerm currentTerm,
    futures::Try<AppendEntriesResult>&& res,
    std::chrono::steady_clock::duration latency, MessageId messageId)
    -> std::pair<std::vector<std::optional<PreparedAppendEntryRequest>>,
                 ResolvedPromiseSet> {
  if (currentTerm != _self._currentTerm) {
    LOG_CTX("7ab2e", WARN, follower.logContext)
        << "received append entries response with wrong term: " << currentTerm;
    return {};
  }

  ResolvedPromiseSet toBeResolved;

  follower._lastRequestLatency = latency;

  if (follower.lastSentMessageId == messageId) {
    LOG_CTX("35a32", TRACE, follower.logContext)
        << "received message " << messageId << " - no other requests in flight";
    // there is no request in flight currently
    follower._state = FollowerInfo::State::IDLE;
  }
  if (res.hasValue()) {
    auto& response = res.get();
    TRI_ASSERT(messageId == response.messageId)
        << messageId << " vs. " << response.messageId;
    if (follower.lastSentMessageId == response.messageId) {
      LOG_CTX("35134", TRACE, follower.logContext)
          << "received append entries response, messageId = "
          << response.messageId
          << ", errorCode = " << to_string(response.errorCode)
          << ", reason  = " << to_string(response.reason.error);

      // We *must* also ignore the snapshot status when the message id is equal.
      // See the comment in the else branch for details.
      if (follower.snapshotAvailableMessageId < response.messageId) {
        if (follower.snapshotAvailable != response.snapshotAvailable) {
          LOG_CTX("efd44", DEBUG, follower.logContext)
              << "snapshot status changed old = " << follower.snapshotAvailable
              << " new = " << response.snapshotAvailable;
          follower.snapshotAvailable = response.snapshotAvailable;
        }
      } else {
        // Note that follower.snapshotAvailableMessageId can be equal to
        // response.messageId. This means that the follower has called
        // update-snapshot-status right after handling the append entries
        // request with that id, but the append entries response arrived here
        // after the update-snapshot-status.
        LOG_CTX("cf587", DEBUG, follower.logContext) << fmt::format(
            "Ignoring snapshot status from append entries response. The "
            "current status ({}) was set with message id {}, while the "
            "response (with status {}) currently being handled has message id "
            "{}.",
            follower.snapshotAvailable, follower.snapshotAvailableMessageId,
            response.snapshotAvailable, response.messageId);
      }

      follower.lastErrorReason = response.reason;
      if (response.isSuccess()) {
        follower.numErrorsSinceLastAnswer = 0;
        follower.lastAckedIndex = lastIndex;
        follower.nextPrevLogIndex = lastIndex.index;
        follower.lastAckedCommitIndex = currentCommitIndex;
        follower.lastAckedLowestIndexToKeep = currentLITK;
      } else {
        TRI_ASSERT(response.reason.error !=
                   AppendEntriesErrorReason::ErrorType::kNone);
        switch (response.reason.error) {
          case AppendEntriesErrorReason::ErrorType::kNoPrevLogMatch:
            follower.numErrorsSinceLastAnswer = 0;
            TRI_ASSERT(response.conflict.has_value());
            follower.nextPrevLogIndex =
                response.conflict.value().index.saturatedDecrement();
            LOG_CTX("33c6d", DEBUG, follower.logContext)
                << "reset last matched index to " << follower.nextPrevLogIndex;
            break;
          default:
            LOG_CTX("1bd0b", DEBUG, follower.logContext)
                << "received error from follower, reason = "
                << to_string(response.reason.error)
                << " message id = " << messageId;
            ++follower.numErrorsSinceLastAnswer;
            _self._logMetrics->replicatedLogLeaderAppendEntriesErrorCount
                ->count();
        }
      }
    } else {
      LOG_CTX("056a8", DEBUG, follower.logContext)
          << "received outdated response from follower "
          << follower._impl->getParticipantId() << ": " << response.messageId
          << ", expected " << messageId << ", latest "
          << follower.lastSentMessageId;
    }
  } else if (res.hasException()) {
    ++follower.numErrorsSinceLastAnswer;
    _self._logMetrics->replicatedLogLeaderAppendEntriesErrorCount->count();
    follower.lastErrorReason = {
        AppendEntriesErrorReason::ErrorType::kCommunicationError};
    try {
      res.throwIfFailed();
    } catch (std::exception const& e) {
      follower.lastErrorReason.details = e.what();
      LOG_CTX("e094b", INFO, follower.logContext)
          << "exception in appendEntries to follower "
          << follower._impl->getParticipantId() << ": " << e.what();
    } catch (...) {
      LOG_CTX("05608", INFO, follower.logContext)
          << "exception in appendEntries to follower "
          << follower._impl->getParticipantId() << ".";
    }
  } else {
    LOG_CTX("dc441", FATAL, follower.logContext)
        << "in appendEntries to follower " << follower._impl->getParticipantId()
        << ", result future has neither value nor exception.";
    TRI_ASSERT(false);
    FATAL_ERROR_EXIT();
  }

  // checkCommitIndex is called regardless of follower response.
  // The follower might be failed, but the agency can't tell that immediately.
  // Thus, we might have to commit an entry without this follower.
  toBeResolved = checkCommitIndex();
  // try sending the next batch
  return std::make_pair(prepareAppendEntries(), std::move(toBeResolved));
}

auto replicated_log::LogLeader::GuardedLeaderData::getInternalLogIterator(
    LogIndex firstIdx) const
    -> std::unique_ptr<TypedLogIterator<InMemoryLogEntry>> {
  if (_inMemoryLog.getFirstIndex() <= firstIdx) {
    auto const endIdx = _inMemoryLog.getLastTermIndexPair().index + 1;
    TRI_ASSERT(firstIdx <= endIdx);
    return _inMemoryLog.getMemtryIteratorFrom(firstIdx);
  }

  auto diskIter = _self._storageManager->getPeristedLogIterator(firstIdx);

  struct OverlayIterator : TypedLogIterator<InMemoryLogEntry> {
    explicit OverlayIterator(
        std::unique_ptr<PersistedLogIterator> diskIter,
        std::unique_ptr<TypedLogIterator<InMemoryLogEntry>> inMemoryIter,
        LogRange inMemoryRange)
        : _diskIter(std::move(diskIter)),
          _inMemoryIter(std::move(inMemoryIter)),
          _inMemoryRange(inMemoryRange) {}

    auto next() -> std::optional<InMemoryLogEntry> override {
      // iterate over the disk until it is covered by the in memory part
      if (_diskIter) {
        auto entry = _diskIter->next();
        if (entry) {
          if (not _inMemoryRange.contains(entry->logIndex())) {
            return InMemoryLogEntry{*entry};
          }
        }
        _diskIter.reset();
      }

      return _inMemoryIter->next();
    }

    std::unique_ptr<PersistedLogIterator> _diskIter;
    std::unique_ptr<TypedLogIterator<InMemoryLogEntry>> _inMemoryIter;
    LogRange _inMemoryRange;
  };

  return std::make_unique<OverlayIterator>(
      std::move(diskIter), _inMemoryLog.getMemtryIteratorFrom(firstIdx),
      _inMemoryLog.getIndexRange());
}

auto replicated_log::LogLeader::GuardedLeaderData::getLogConsumerIterator(
    std::optional<LogRange> bounds) const -> std::unique_ptr<LogRangeIterator> {
  // Note that there can be committed log entries only in memory, because they
  // might not be persisted locally.

  // Intersect the range with the committed range
  auto range = LogRange{LogIndex{0}, _commitIndex + 1};
  if (bounds) {
    range = intersect(range, *bounds);
  }

  // check if we can serve everything from memory
  if (_inMemoryLog.getIndexRange().contains(range)) {
    return _inMemoryLog.getIteratorRange(range);
  }

  // server from disk
  auto diskIter = _self._storageManager->getCommittedLogIterator(range);

  struct OverlayIterator : LogRangeIterator {
    explicit OverlayIterator(std::unique_ptr<LogRangeIterator> diskIter,
                             std::unique_ptr<LogRangeIterator> inMemoryIter,
                             LogRange inMemoryRange, LogRange range)
        : _diskIter(std::move(diskIter)),
          _inMemoryIter(std::move(inMemoryIter)),
          _inMemoryRange(inMemoryRange),
          _range(range) {}

    auto next() -> std::optional<LogEntryView> override {
      // iterate over the disk until it is covered by the in memory part
      if (_diskIter) {
        auto entry = _diskIter->next();
        if (entry) {
          if (not _inMemoryRange.contains(entry->logIndex())) {
            return entry;
          }
        }
        _diskIter.reset();
      }

      return _inMemoryIter->next();
    }

    auto range() const noexcept -> LogRange override { return _range; }

    std::unique_ptr<LogRangeIterator> _diskIter;
    std::unique_ptr<LogRangeIterator> _inMemoryIter;
    LogRange _inMemoryRange;
    LogRange _range;
  };

  return std::make_unique<OverlayIterator>(std::move(diskIter),
                                           _inMemoryLog.getIteratorRange(range),
                                           _inMemoryLog.getIndexRange(), range);
}

/*
 * Collects last acknowledged term/index pairs from all followers.
 * While doing so, it calculates the largest common index, which is
 * the lowest acknowledged index of all followers.
 * No followers are filtered out at this step.
 */
auto replicated_log::LogLeader::GuardedLeaderData::collectFollowerStates() const
    -> std::pair<LogIndex, std::vector<algorithms::ParticipantState>> {
  auto largestCommonIndex = _commitIndex;
  std::vector<algorithms::ParticipantState> participantStates;
  participantStates.reserve(_follower.size());
  for (auto const& [pid, follower] : _follower) {
    // The lastAckedEntry is the last index/term pair that we sent that this
    // follower acknowledged - means we sent it. And we must not have entries
    // in our log with a term newer than currentTerm, which could have been
    // sent to a follower.
    TRI_ASSERT(follower->lastAckedIndex.term <= this->_self._currentTerm);

    auto flags = activeParticipantsConfig->participants.find(pid);
    TRI_ASSERT(flags != std::end(activeParticipantsConfig->participants));
    participantStates.emplace_back(algorithms::ParticipantState{
        .lastAckedEntry = follower->lastAckedIndex,
        .id = pid,
        .snapshotAvailable = follower->snapshotAvailable,
        .flags = flags->second});

    largestCommonIndex =
        std::min(largestCommonIndex, follower->lastAckedIndex.index);
  }

  return {largestCommonIndex, std::move(participantStates)};
}

auto replicated_log::LogLeader::GuardedLeaderData::checkCommitIndex()
    -> ResolvedPromiseSet {
  auto [largestCommonIndex, indexes] = collectFollowerStates();

  auto [releaseIndex, lowestIndexToKeep] =
      _self._compactionManager->getIndexes();
  if (largestCommonIndex > lowestIndexToKeep) {
    LOG_CTX("851bb", TRACE, _self._logContext)
        << "largest common index went from " << lowestIndexToKeep << " to "
        << largestCommonIndex;
    _self._compactionManager->updateLowestIndexToKeep(largestCommonIndex);
  }

  auto [newCommitIndex, commitFailReason, quorum] =
      algorithms::calculateCommitIndex(
          indexes, this->activeParticipantsConfig->config.effectiveWriteConcern,
          _commitIndex, _inMemoryLog.getLastTermIndexPair());
  _lastCommitFailReason = commitFailReason;

  LOG_CTX("6a6c0", TRACE, _self._logContext)
      << "calculated commit index as " << newCommitIndex
      << ", current commit index = " << _commitIndex;
  LOG_CTX_IF("fbc23", TRACE, _self._logContext, newCommitIndex == _commitIndex)
      << "commit fail reason = " << to_string(commitFailReason)
      << " follower-states = " << indexes;
  if (newCommitIndex > _commitIndex) {
    auto const quorum_data = std::make_shared<QuorumData>(
        newCommitIndex, _self._currentTerm, std::move(quorum));
    return updateCommitIndexLeader(newCommitIndex, quorum_data);
  }
  return {};
}

auto replicated_log::LogLeader::GuardedLeaderData::getLocalStatistics() const
    -> LogStatistics {
  auto [releaseIndex, lowestIndexToKeep] =
      _self._compactionManager->getIndexes();
  auto mapping = _self._storageManager->getTermIndexMapping();
  auto result = LogStatistics{};
  result.commitIndex = _commitIndex;
  result.firstIndex = mapping.getFirstIndex().value_or(TermIndexPair{}).index;
  result.spearHead = _inMemoryLog.getLastTermIndexPair();
  result.releaseIndex = releaseIndex;
  return result;
}

replicated_log::LogLeader::GuardedLeaderData::GuardedLeaderData(
    replicated_log::LogLeader& self,
    std::unique_ptr<IReplicatedStateHandle> stateHandle, LogIndex firstIndex)
    : _self(self),
      _inMemoryLog(firstIndex),
      _stateHandle(std::move(stateHandle)) {}

auto replicated_log::LogLeader::release(LogIndex doneWithIdx) -> Result {
  _compactionManager->updateReleaseIndex(doneWithIdx);
  return {};
}

auto replicated_log::LogLeader::compact() -> ResultT<CompactionResult> {
  auto result = _compactionManager->compact().get();
  if (result.error) {
    return Result{result.error->errorNumber(), result.error->errorMessage()};
  }
  return CompactionResult{.numEntriesCompacted = result.compactedRange.count(),
                          .range = result.compactedRange,
                          .stopReason = result.stopReason};
}

auto replicated_log::LogLeader::GuardedLeaderData::calculateCommitLag()
    const noexcept -> std::chrono::duration<double, std::milli> {
  auto memtry = _inMemoryLog.getEntryByIndex(_commitIndex + 1);
  if (memtry.has_value()) {
    return std::chrono::duration_cast<
        std::chrono::duration<double, std::milli>>(
        std::chrono::steady_clock::now() - memtry->insertTp());
  } else {
    TRI_ASSERT(_commitIndex == LogIndex{0} ||
               _commitIndex == _inMemoryLog.getLastIndex())
        << "If there is no entry following the commitIndex the last index "
           "should be the commitIndex. _commitIndex = "
        << _commitIndex << ", lastIndex = " << _inMemoryLog.getLastIndex();
    return {};
  }
}

auto replicated_log::LogLeader::GuardedLeaderData::waitForResign()
    -> std::pair<futures::Future<futures::Unit>, DeferredAction> {
  if (!_didResign) {
    auto future = _waitForResignQueue.addWaitFor();
    return {std::move(future), DeferredAction{}};
  } else {
    TRI_ASSERT(_waitForResignQueue.empty());
    auto promise = futures::Promise<futures::Unit>{};
    auto future = promise.getFuture();

    auto action =
        DeferredAction([promise = std::move(promise)]() mutable noexcept {
          TRI_ASSERT(promise.valid());
          promise.setValue();
        });

    return {std::move(future), std::move(action)};
  }
}

auto replicated_log::LogLeader::getReplicatedLogSnapshot() const
    -> InMemoryLog::log_type {
  auto [log, commitIndex] =
      _guardedLeaderData.doUnderLock([](auto const& leaderData) {
        if (leaderData._didResign) {
          throw ParticipantResignedException(
              TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
        }

        return std::make_pair(leaderData._inMemoryLog, leaderData._commitIndex);
      });

  return log.takeSnapshotUpToAndIncluding(commitIndex).copyFlexVector();
}

auto replicated_log::LogLeader::waitForIterator(LogIndex index)
    -> replicated_log::ILogParticipant::WaitForIteratorFuture {
  if (index == LogIndex{0}) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "invalid parameter; log index 0 is invalid");
  }

  return waitFor(index).thenValue([this, self = shared_from_this(), index](
                                      auto&& quorum) -> WaitForIteratorFuture {
    auto [actualIndex, iter] = _guardedLeaderData.doUnderLock(
        [index](GuardedLeaderData& leaderData)
            -> std::pair<LogIndex, std::unique_ptr<LogRangeIterator>> {
          TRI_ASSERT(index <= leaderData._commitIndex);

          /*
           * This code here ensures that if only private log entries are present
           * we do not reply with an empty iterator but instead wait for the
           * next entry containing payload.
           */

          auto testIndex = index;
          while (testIndex <= leaderData._commitIndex) {
            auto memtry = leaderData._inMemoryLog.getEntryByIndex(testIndex);
            if (!memtry.has_value()) {
              break;
            }
            if (memtry->entry().hasPayload()) {
              break;
            }
            testIndex = testIndex + 1;
          }

          if (testIndex > leaderData._commitIndex) {
            return std::make_pair(testIndex, nullptr);
          }

          return std::make_pair(
              testIndex, leaderData.getLogConsumerIterator(
                             LogRange{testIndex, leaderData._commitIndex + 1}));
        });

    // call here, otherwise we deadlock with waitFor
    if (iter == nullptr) {
      return waitForIterator(actualIndex);
    }

    return std::move(iter);
  });
}

auto replicated_log::LogLeader::getLogConsumerIterator(
    std::optional<LogRange> bounds) const -> std::unique_ptr<LogRangeIterator> {
  return _guardedLeaderData.getLockedGuard()->getLogConsumerIterator(bounds);
}

replicated_log::LogLeader::LocalFollower::LocalFollower(
    replicated_log::LogLeader& self, LoggerContext logContext,
    std::shared_ptr<IStorageManager> storageManager,
    [[maybe_unused]] TermIndexPair lastIndex)
    : _leader(self),
      _logContext(std::move(logContext)),
      _storageManager(std::move(storageManager)) {}

auto replicated_log::LogLeader::LocalFollower::getParticipantId() const noexcept
    -> ParticipantId const& {
  return _leader.getParticipantId();
}

auto replicated_log::LogLeader::LocalFollower::appendEntries(
    AppendEntriesRequest const request)
    -> futures::Future<AppendEntriesResult> {
  MeasureTimeGuard measureTimeGuard(
      *_leader._logMetrics->replicatedLogFollowerAppendEntriesRtUs);

  auto messageLogContext =
      _logContext.with<logContextKeyMessageId>(request.messageId)
          .with<logContextKeyPrevLogIdx>(request.prevLogEntry.index)
          .with<logContextKeyPrevLogTerm>(request.prevLogEntry.term)
          .with<logContextKeyLeaderCommit>(request.leaderCommit);

  auto returnAppendEntriesResult =
      [term = request.leaderTerm, messageId = request.messageId,
       logContext = messageLogContext,
       measureTime = std::move(measureTimeGuard)](Result const& res) mutable {
        // fire here because the lambda is destroyed much later in a future
        measureTime.fire();
        if (!res.ok()) {
          LOG_CTX("fdc87", FATAL, logContext)
              << "local follower failed to write entries: " << res;
          FATAL_ERROR_EXIT();
        }
        LOG_CTX("e0800", TRACE, logContext)
            << "local follower completed append entries";
        return AppendEntriesResult{term, messageId, true};
      };

  LOG_CTX("6fa8b", TRACE, messageLogContext)
      << "local follower received append entries";

  if (request.entries.empty()) {
    // Nothing to do here, save some work.
    return returnAppendEntriesResult(Result(TRI_ERROR_NO_ERROR));
  }

  // Note that the beginning of iter here is always (and must be) exactly
  // the next index after the last one in the LogCore.
  replicated_state::IStorageEngineMethods::WriteOptions opts;
  opts.waitForSync = request.waitForSync;
  auto trx = _storageManager->transaction();
  return trx->appendEntries(InMemoryLog{request.entries})
      .thenValue(std::move(returnAppendEntriesResult));
}

void replicated_log::LogLeader::establishLeadership(
    std::shared_ptr<agency::ParticipantsConfig const> config) {
  LOG_CTX("f3aa8", TRACE, _logContext) << "trying to establish leadership";
  auto waitForIndex =
      _guardedLeaderData.doUnderLock([&](GuardedLeaderData& data) {
        // Immediately append an empty log entry in the new term. This is
        // necessary because we must not commit entries of older terms, but do
        // not want to wait with committing until the next insert.

        // Also make sure that this entry is written with waitForSync = true
        // to ensure that entries of the previous term are synced as well.
        auto meta = LogMetaPayload::FirstEntryOfTerm{.leader = data._self._id,
                                                     .participants = *config};
        auto firstIndex = data.insertInternal(LogMetaPayload{std::move(meta)},
                                              true, std::nullopt);
        return firstIndex;
      });
  TRI_ASSERT(waitForIndex == _firstIndexOfCurrentTerm)
      << "got waitForIndex = " << waitForIndex
      << " but firstIndexOfCurrentTerm is " << _firstIndexOfCurrentTerm;
  waitFor(waitForIndex)
      .thenFinal([weak = weak_from_this(), config = std::move(config)](
                     futures::Try<WaitForResult>&& result) mutable noexcept {
        if (auto self = weak.lock(); self) {
          try {
            result.throwIfFailed();
            self->_guardedLeaderData.doUnderLock([&](auto& data) {
              data._leadershipEstablished = true;
              if (data.activeParticipantsConfig->generation ==
                  config->generation) {
                data.committedParticipantsConfig = std::move(config);
              }
            });
            LOG_CTX("536f4", TRACE, self->_logContext)
                << "leadership established";
          } catch (ParticipantResignedException const& err) {
            LOG_CTX("22264", TRACE, self->_logContext)
                << "failed to establish leadership due to resign: "
                << err.what();
          } catch (std::exception const& err) {
            LOG_CTX("5ceda", FATAL, self->_logContext)
                << "failed to establish leadership: " << err.what();
          }
        } else {
          LOG_TOPIC("94696", TRACE, Logger::REPLICATION2)
              << "leader is already gone, no leadership was established";
        }
      });
}

auto replicated_log::LogLeader::waitForLeadership()
    -> replicated_log::ILogParticipant::WaitForFuture {
  return waitFor(_firstIndexOfCurrentTerm);
}

namespace {
// For (unordered) maps `left` and `right`, return `keys(left)
// \ keys(right)`
auto const keySetDifference = [](auto const& left, auto const& right) {
  using left_t = std::decay_t<decltype(left)>;
  using right_t = std::decay_t<decltype(right)>;
  static_assert(
      std::is_same_v<typename left_t::key_type, typename right_t::key_type>);
  using key_t = typename left_t::key_type;

  auto result = std::vector<key_t>{};
  for (auto const& [key, val] : left) {
    if (!right.contains(key)) {
      result.emplace_back(key);
    }
  }

  return result;
};
}  // namespace

auto replicated_log::LogLeader::updateParticipantsConfig(
    std::shared_ptr<agency::ParticipantsConfig const> const& config)
    -> LogIndex {
  LOG_CTX("ac277", TRACE, _logContext)
      << "trying to update configuration to generation " << config->generation;
  auto waitForIndex = _guardedLeaderData.doUnderLock([&](GuardedLeaderData&
                                                             data) {
    auto const [followersToRemove, additionalFollowers] = std::invoke([&] {
      auto const& oldFollowers = data._follower;
      // Note that newParticipants contains the leader, while
      // oldFollowers does not.
      auto const& newParticipants = config->participants;
      auto const additionalParticipantIds =
          keySetDifference(newParticipants, oldFollowers);
      auto followersToRemove_ = keySetDifference(oldFollowers, newParticipants);

      auto additionalFollowers_ =
          std::unordered_map<ParticipantId,
                             std::shared_ptr<AbstractFollower>>{};
      for (auto const& participantId : additionalParticipantIds) {
        // exclude the leader
        if (participantId != _id) {
          additionalFollowers_.try_emplace(
              participantId,
              _followerFactory->constructFollower(participantId));
        }
      }
      return std::pair(std::move(followersToRemove_),
                       std::move(additionalFollowers_));
    });

    if (data.activeParticipantsConfig->generation >= config->generation) {
      auto const message = basics::StringUtils::concatT(
          "updated participant config generation is smaller or equal to "
          "current generation - refusing to update; ",
          "new = ", config->generation,
          ", current = ", data.activeParticipantsConfig->generation);
      LOG_CTX("bab5b", TRACE, _logContext) << message;
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, message);
    }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // all participants in the new configuration must either exist
    // already, or be added via additionalFollowers.
    {
      auto const& newConfigParticipants = config->participants;
      TRI_ASSERT(std::all_of(
          newConfigParticipants.begin(), newConfigParticipants.end(),
          [&, &additionalFollowers = additionalFollowers](auto const& it) {
            return data._follower.contains(it.first) ||
                   additionalFollowers.contains(it.first) ||
                   it.first == data._self.getParticipantId();
          }));
    }
#endif

    // Create a copy. This is important to keep the following code
    // exception-safe, in particular never leave data._follower behind in
    // a half-updated state.
    auto followers = data._follower;

    {  // remove obsolete followers
      for (auto const& it : followersToRemove) {
        followers.erase(it);
      }
    }
    {  // add new followers
      for (auto&& [participantId, abstractFollowerPtr] : additionalFollowers) {
        auto const lastIndex =
            data._inMemoryLog.getLastTermIndexPair().index.saturatedDecrement();
        followers.try_emplace(
            participantId,
            std::make_shared<FollowerInfo>(std::move(abstractFollowerPtr),
                                           lastIndex, data._self._logContext));
      }
    }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // all participants (but the leader) in the new configuration must now
    // be part of followers
    {
      auto const& newConfigParticipants = config->participants;
      TRI_ASSERT(std::all_of(newConfigParticipants.begin(),
                             newConfigParticipants.end(), [&](auto const& it) {
                               return followers.contains(it.first) ||
                                      it.first == data._self.getParticipantId();
                             }));
    }
#endif

    auto meta =
        LogMetaPayload::UpdateParticipantsConfig{.participants = *config};
    auto const idx = data.insertInternal(LogMetaPayload{std::move(meta)}, true,
                                         std::nullopt);
    data.activeParticipantsConfig = config;
    data._follower.swap(followers);

    return idx;
  });

  triggerAsyncReplication();
  waitFor(waitForIndex)
      .thenFinal([weak = weak_from_this(),
                  config](futures::Try<WaitForResult>&& result) noexcept {
        if (auto self = weak.lock(); self) {
          try {
            result.throwIfFailed();
            if (auto guard = self->_guardedLeaderData.getLockedGuard();
                guard->activeParticipantsConfig->generation ==
                config->generation) {
              // Make sure config is the currently active configuration. It
              // could happen that activeParticipantsConfig was changed before
              // config got any chance to see anything committed, thus never
              // being considered an actual committedParticipantsConfig. In
              // this case we skip it.
              guard->committedParticipantsConfig = config;
              LOG_CTX("536f5", DEBUG, self->_logContext)
                  << "configuration committed, generation "
                  << config->generation;
            } else {
              LOG_CTX("fd245", TRACE, self->_logContext)
                  << "configuration already newer than generation "
                  << config->generation;
            }
          } catch (ParticipantResignedException const& err) {
            LOG_CTX("3959f", DEBUG, self->_logContext)
                << "leader resigned before new participant configuration was "
                   "committed: "
                << err.message();
          } catch (std::exception const& err) {
            LOG_CTX("1af0f", FATAL, self->_logContext)
                << "failed to commit new participant config; " << err.what();
            FATAL_ERROR_EXIT();  // TODO is there nothing we can do here?
          }
        }

        LOG_TOPIC("a4fc1", TRACE, Logger::REPLICATION2)
            << "leader is already gone, configuration change was not "
               "committed";
      });

  return waitForIndex;
}

auto replicated_log::LogLeader::getCommitIndex() const noexcept -> LogIndex {
  return _guardedLeaderData.getLockedGuard()->_commitIndex;
}

auto replicated_log::LogLeader::getParticipantConfigGenerations() const noexcept
    -> std::pair<std::size_t, std::optional<std::size_t>> {
  return _guardedLeaderData.doUnderLock([&](GuardedLeaderData const& data) {
    auto activeGeneration = data.activeParticipantsConfig->generation;
    auto committedGeneration = std::optional<std::size_t>{};

    if (auto committedConfig = data.committedParticipantsConfig;
        committedConfig != nullptr) {
      committedGeneration = committedConfig->generation;
    }

    return std::make_pair(activeGeneration, committedGeneration);
  });
}

auto replicated_log::LogLeader::setSnapshotAvailable(
    ParticipantId const& participantId, SnapshotAvailableReport report)
    -> Result {
  auto guard = _guardedLeaderData.getLockedGuard();
  if (guard->_didResign) {
    throw ParticipantResignedException(
        TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
  }
  auto follower = guard->_follower.find(participantId);
  if (follower == guard->_follower.end()) {
    return {TRI_ERROR_CLUSTER_NOT_FOLLOWER};
  }
  auto& followerInfo = *follower->second;
  if (followerInfo.snapshotAvailableMessageId > report.messageId) {
    // We already got more recent information, we may silently ignore this.
    // NOTE that '==' instead of '>' *must not* be ignored: An
    // AppendEntriesResponse can have the same MessageId as an
    // "update-snapshot-status", but is always less recent.
    LOG_CTX("62dc4", DEBUG, _logContext) << fmt::format(
        "Ignoring outdated 'snapshot available' message from {} follower. "
        "This was reported with message id {}, but we already have a report "
        "from {}. The current status is {}.",
        participantId, report.messageId,
        followerInfo.snapshotAvailableMessageId,
        followerInfo.snapshotAvailable);
    return {};
  }
  followerInfo.snapshotAvailable = true;
  followerInfo.snapshotAvailableMessageId = report.messageId;
  LOG_CTX("c8b6a", INFO, _logContext)
      << "Follower snapshot " << participantId << " completed.";
  auto promises = guard->checkCommitIndex();
  guard.unlock();
  handleResolvedPromiseSet(_scheduler.get(), std::move(promises), _logMetrics);
  return {};
}

auto replicated_log::LogLeader::ping(std::optional<std::string> message)
    -> LogIndex {
  auto index = _guardedLeaderData.doUnderLock([&](GuardedLeaderData& leader) {
    auto meta = LogMetaPayload::withPing(message);
    return leader.insertInternal(std::move(meta), false, std::nullopt);
  });

  triggerAsyncReplication();
  return index;
}
auto replicated_log::LogLeader::resign() && -> std::tuple<
    std::unique_ptr<replicated_state::IStorageEngineMethods>,
    std::unique_ptr<IReplicatedStateHandle>, DeferredAction> {
  auto [actionOuter, leaderEstablished, stateHandle] =
      _guardedLeaderData.doUnderLock([this, &localFollower = *_localFollower,
                                      &participantId =
                                          _id](GuardedLeaderData& leaderData) {
        if (leaderData._didResign) {
          LOG_CTX("5d3b8", ERR, _logContext)
              << "Leader " << participantId << " already resigned!";
          throw ParticipantResignedException(
              TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED, ADB_HERE);
        }

        // WARNING! This stunt is here to make things exception safe.
        // The move constructor of std::multimap is **not** noexcept.
        // Thus we have to make a new map unique and use std::swap to
        // transfer the content. And then move the unique_ptr into
        // the lambda.
        struct Queues {
          WaitForQueue waitForQueue;
          WaitForBag waitForResignQueue;
        };
        auto queues = std::make_unique<Queues>();
        std::swap(queues->waitForQueue, leaderData._waitForQueue);
        queues->waitForResignQueue = std::move(leaderData._waitForResignQueue);
        auto action = [queues = std::move(queues)]() mutable noexcept {
          for (auto& [idx, promise] : queues->waitForQueue) {
            // Check this to make sure that setException does not throw
            if (!promise.isFulfilled()) {
              promise.setException(ParticipantResignedException(
                  TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED,
                  ADB_HERE));
            }
          }
          queues->waitForResignQueue.resolveAll();
        };

        LOG_CTX("8696f", DEBUG, _logContext) << "resign";
        leaderData._didResign = true;
        static_assert(
            std::is_nothrow_constructible_v<
                DeferredAction, std::add_rvalue_reference_t<decltype(action)>>);
        return std::make_tuple(DeferredAction(std::move(action)),
                               leaderData._leadershipEstablished,
                               std::move(leaderData._stateHandle));
      });
  if (leaderEstablished) {
    auto methods = stateHandle->resignCurrentState();
    ADB_PROD_ASSERT(methods != nullptr);
  }
  static_assert(noexcept(std::declval<StorageManager>().resign()));
  auto core =
      std::static_pointer_cast<StorageManager>(_storageManager)->resign();
  return std::make_tuple(std::move(core), std::move(stateHandle),
                         std::move(actionOuter));
}

auto replicated_log::LogLeader::getInternalLogIterator(
    std::optional<LogRange> bounds) const
    -> std::unique_ptr<PersistedLogIterator> {
  auto range =
      bounds ? *bounds : LogRange{LogIndex{0}, LogIndex{std::uint64_t(-1)}};
  auto iter =
      _guardedLeaderData.getLockedGuard()->getInternalLogIterator(range.from);

  struct Adapter : PersistedLogIterator {
    explicit Adapter(std::unique_ptr<TypedLogIterator<InMemoryLogEntry>> iter,
                     LogRange range)
        : iter(std::move(iter)), range(range) {}

    auto next() -> std::optional<PersistingLogEntry> override {
      auto entry = iter->next();
      if (entry && range.contains(entry->entry().logIndex())) {
        return entry->entry();
      }
      return std::nullopt;
    }

    std::unique_ptr<TypedLogIterator<InMemoryLogEntry>> iter;
    LogRange range;
  };

  return std::make_unique<Adapter>(std::move(iter), range);
}

auto replicated_log::LogLeader::LocalFollower::release(LogIndex stop) const
    -> Result {
  LOG_CTX("23745", DEBUG, _logContext)
      << "local follower releasing with stop at " << stop;
  auto trx = _storageManager->transaction();
  auto res = trx->removeFront(stop).get();
  LOG_CTX_IF("2aba1", WARN, _logContext, res.fail())
      << "local follower failed to release log entries: " << res.errorMessage();
  return res;
}

replicated_log::LogLeader::PreparedAppendEntryRequest::
    PreparedAppendEntryRequest(
        std::shared_ptr<LogLeader> const& logLeader,
        std::shared_ptr<FollowerInfo> follower,
        std::chrono::steady_clock::duration executionDelay)
    : _parentLog(logLeader),
      _follower(std::move(follower)),
      _executionDelay(executionDelay) {}

replicated_log::LogLeader::FollowerInfo::FollowerInfo(
    std::shared_ptr<AbstractFollower> impl, LogIndex lastLogIndex,
    LoggerContext const& logContext)
    : _impl(std::move(impl)),
      nextPrevLogIndex(lastLogIndex),
      logContext(
          logContext.with<logContextKeyLogComponent>("follower-info")
              .with<logContextKeyFollowerId>(_impl->getParticipantId())) {}
