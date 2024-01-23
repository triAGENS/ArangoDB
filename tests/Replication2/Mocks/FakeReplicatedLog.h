////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once
#include <deque>
#include <memory>

#include "Replication2/Mocks/ReplicatedLogMetricsMock.h"
#include "Replication2/ReplicatedLog/ILogInterfaces.h"
#include "Replication2/ReplicatedLog/InMemoryLog.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/LogCore.h"
#include "Replication2/ReplicatedLog/LogFollower.h"
#include "Replication2/ReplicatedLog/LogLeader.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"

namespace arangodb::replication2::test {

struct DelayedFollowerLog : replicated_log::AbstractFollower,
                            replicated_log::ILogParticipant {
  explicit DelayedFollowerLog(
      std::shared_ptr<replicated_log::LogFollower> follower)
      : _follower(std::move(follower)) {}

  DelayedFollowerLog(LoggerContext const& logContext,
                     std::shared_ptr<ReplicatedLogMetricsMock> logMetricsMock,
                     std::shared_ptr<ReplicatedLogGlobalSettings const> options,
                     ParticipantId const& id,
                     std::unique_ptr<replicated_log::LogCore> logCore,
                     LogTerm term, ParticipantId leaderId)
      : DelayedFollowerLog([&] {
          return replicated_log::LogFollower::construct(
              logContext, std::move(logMetricsMock), std::move(options), id,
              std::move(logCore), term, std::move(leaderId));
        }()) {}

  auto appendEntries(replicated_log::AppendEntriesRequest req)
      -> arangodb::futures::Future<
          replicated_log::AppendEntriesResult> override {
    auto future = _asyncQueue.doUnderLock([&](auto& queue) {
      return queue.emplace_back(std::make_shared<AsyncRequest>(std::move(req)))
          ->promise.getFuture();
    });
    return std::move(future).thenValue([this](auto&& result) mutable {
      return _follower->appendEntries(std::forward<decltype(result)>(result));
    });
  }

  void runAsyncAppendEntries() {
    auto asyncQueue = _asyncQueue.doUnderLock([](auto& _queue) {
      auto queue = std::move(_queue);
      _queue.clear();
      return queue;
    });

    for (auto& p : asyncQueue) {
      p->promise.setValue(std::move(p->request));
    }
  }

  void runAllAsyncAppendEntries() {
    while (hasPendingAppendEntries()) {
      runAsyncAppendEntries();
    }
  }

  auto getCommitIndex() const noexcept -> LogIndex override {
    return _follower->getCommitIndex();
  }

  auto copyInMemoryLog() const -> replicated_log::InMemoryLog override {
    return _follower->copyInMemoryLog();
  }

  using WaitForAsyncPromise =
      futures::Promise<replicated_log::AppendEntriesRequest>;

  struct AsyncRequest {
    explicit AsyncRequest(replicated_log::AppendEntriesRequest request)
        : request(std::move(request)) {}
    replicated_log::AppendEntriesRequest request;
    WaitForAsyncPromise promise;
  };
  [[nodiscard]] auto pendingAppendEntries() const
      -> std::deque<std::shared_ptr<AsyncRequest>> {
    return _asyncQueue.copy();
  }
  [[nodiscard]] auto hasPendingAppendEntries() const -> bool {
    return _asyncQueue.doUnderLock(
        [](auto const& queue) { return !queue.empty(); });
  }

  auto getParticipantId() const noexcept -> ParticipantId const& override {
    return _follower->getParticipantId();
  }

  auto getStatus() const -> replicated_log::LogStatus override {
    return _follower->getStatus();
  }

  auto getQuickStatus() const -> replicated_log::QuickLogStatus override {
    return _follower->getQuickStatus();
  }

  [[nodiscard]] auto resign() && -> std::tuple<
      std::unique_ptr<replicated_log::LogCore>, DeferredAction> override {
    return std::move(*_follower).resign();
  }

  auto waitFor(LogIndex index) -> WaitForFuture override {
    return _follower->waitFor(index);
  }

  auto waitForIterator(LogIndex index) -> WaitForIteratorFuture override {
    return _follower->waitForIterator(index);
  }
  auto waitForResign() -> futures::Future<futures::Unit> override {
    return _follower->waitForResign();
  }
  auto release(LogIndex doneWithIdx) -> Result override {
    return _follower->release(doneWithIdx);
  }
  auto compact() -> Result override { return _follower->compact(); }

 private:
  Guarded<std::deque<std::shared_ptr<AsyncRequest>>> _asyncQueue;
  std::shared_ptr<replicated_log::LogFollower> _follower;
};

struct TestReplicatedLog : replicated_log::ReplicatedLog {
  using ReplicatedLog::becomeLeader;
  using ReplicatedLog::ReplicatedLog;
  TestReplicatedLog()
      : ReplicatedLog(
            std::unique_ptr<LogCore>(), std::shared_ptr<ReplicatedLogMetrics>(),
            std::shared_ptr<const ReplicatedLogGlobalSettings>(),
            <#initializer #>,
            arangodb::replication2::agency::ServerInstanceReference()) {}
  auto becomeFollower(ParticipantId const& id, LogTerm term,
                      ParticipantId leaderId)
      -> std::shared_ptr<DelayedFollowerLog>;

  auto becomeLeader(
      ParticipantId const& id, LogTerm term,
      std::vector<std::shared_ptr<replicated_log::AbstractFollower>> const&,
      std::size_t writeConcern, bool waitForSync = false,
      std::shared_ptr<cluster::IFailureOracle> failureOracle = nullptr)
      -> std::shared_ptr<replicated_log::LogLeader>;
};
}  // namespace arangodb::replication2::test
