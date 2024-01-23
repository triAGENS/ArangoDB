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

#include "Basics/voc-errors.h"

#include "Replication2/ReplicatedLog/ReplicatedLog.h"

namespace arangodb::replication2::test {

/**
 * This class queue all append entries requests and allows to expect them.
 * You can then define a custom response to those append entries requests.
 *
 * It only models an AbstractFollower. If you want to have full control,
 * consider using the FakeFollower.
 */
struct FakeAbstractFollower : replicated_log::AbstractFollower, IHasScheduler {
  explicit FakeAbstractFollower(ParticipantId id)
      : participantId(std::move(id)) {}

  [[nodiscard]] auto getParticipantId() const noexcept
      -> ParticipantId const& override {
    return participantId;
  };

  auto appendEntries(replicated_log::AppendEntriesRequest request)
      -> futures::Future<replicated_log::AppendEntriesResult> override {
    return requests.emplace_back(std::move(request)).promise.getFuture();
  }

  void resolveRequest(replicated_log::AppendEntriesResult result) {
    requests.front().promise.setValue(std::move(result));
    requests.pop_front();
  }
  auto hasWork() const noexcept -> bool override {
    return hasPendingRequests();
  }
  auto runAll() noexcept -> std::size_t override {
    auto count = std::size_t{0};
    while (hasPendingRequests()) {
      ++count;
      resolveWithOk();
    }
    return count;
  }

  void resolveWithOk() {
    resolveRequest(
        replicated_log::AppendEntriesResult{currentRequest().leaderTerm,
                                            TRI_ERROR_NO_ERROR,
                                            {},
                                            currentRequest().messageId,
                                            snapshotStatus,
                                            syncIndex});
  }

  void setSyncIndex(LogIndex index) {
    TRI_ASSERT(index >= syncIndex);
    syncIndex = index;
  }

  template<typename E>
  void resolveRequestWithException(E&& e) {
    requests.front().promise.setException(std::forward<E>(e));
    requests.pop_front();
  }

  [[nodiscard]] auto currentRequest() const
      -> replicated_log::AppendEntriesRequest const& {
    return requests.front().request;
  }

  [[nodiscard]] auto hasPendingRequests() const -> bool {
    return !requests.empty();
  }

  void handleAllRequestsWithOk() {
    while (hasPendingRequests()) {
      resolveWithOk();
    }
  }

  struct AsyncRequest {
    explicit AsyncRequest(replicated_log::AppendEntriesRequest request)
        : request(std::move(request)) {}
    replicated_log::AppendEntriesRequest request;
    futures::Promise<replicated_log::AppendEntriesResult> promise;
  };

  std::deque<AsyncRequest> requests;
  ParticipantId participantId;
  bool snapshotStatus{true};
  LogIndex syncIndex = LogIndex{0};
};
}  // namespace arangodb::replication2::test
