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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "LeaderStateManager.h"

#include "Replication2/MetricsHelper.h"
#include "Replication2/ReplicatedState/LazyDeserializingIterator.h"
#include "Replication2/ReplicatedState/ReplicatedStateMetrics.h"

namespace arangodb::replication2::replicated_state {

template<typename S>
void LeaderStateManager<S>::recoverEntries() {
  LOG_CTX("1b3d0", DEBUG, _loggerContext) << "starting recovery";
  auto future = _guardedData.getLockedGuard()->recoverEntries();
  std::move(future).thenFinal(
      [weak = this->weak_from_this()](futures::Try<Result>&& tryResult) {
        // TODO error handling
        ADB_PROD_ASSERT(tryResult.hasValue());
        ADB_PROD_ASSERT(tryResult.get().ok()) << tryResult.get();
        if (auto self = weak.lock(); self != nullptr) {
          auto guard = self->_guardedData.getLockedGuard();
          guard->_leaderState->onRecoveryCompleted();
          guard->_recoveryCompleted = true;
          LOG_CTX("1b246", INFO, self->_loggerContext) << "recovery completed";
        }
      });
}

template<typename S>
auto LeaderStateManager<S>::GuardedData::recoverEntries() {
  auto logIter = _stream->methods()->getCommittedLogIterator();
  auto deserializedIter =
      std::make_unique<LazyDeserializingIterator<EntryType, Deserializer>>(
          std::move(logIter));
  MeasureTimeGuard timeGuard(*_metrics.replicatedStateRecoverEntriesRtt);
  // cppcheck-suppress accessMoved ; deserializedIter is only accessed once
  return _leaderState->recoverEntries(std::move(deserializedIter))
      .then([guard = std::move(timeGuard)](auto&& res) mutable {
        guard.fire();
        return std::move(res.get());
      });
}

template<typename S>
LeaderStateManager<S>::LeaderStateManager(
    LoggerContext loggerContext,
    std::shared_ptr<ReplicatedStateMetrics> metrics,
    std::shared_ptr<IReplicatedLeaderState<S>> leaderState,
    std::shared_ptr<StreamImpl> stream)
    : _loggerContext(std::move(loggerContext)),
      _metrics(std::move(metrics)),
      _guardedData{_loggerContext, *_metrics, std::move(leaderState),
                   std::move(stream)} {
  ADB_PROD_ASSERT(_guardedData.getLockedGuard()->_stream != nullptr);
}

template<typename S>
auto LeaderStateManager<S>::resign() && noexcept
    -> std::pair<std::unique_ptr<CoreType>,
                 std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>> {
  return std::move(_guardedData.getLockedGuard().get()).resign();
}

template<typename S>
auto LeaderStateManager<S>::getInternalStatus() const -> Status::Leader {
  auto guard = _guardedData.getLockedGuard();
  if (guard->_leaderState == nullptr) {
    // we have already resigned
    return {Status::Leader::Resigned{}};
  } else if (guard->_recoveryCompleted) {
    return {Status::Leader::Operational{}};
  } else {
    return {Status::Leader::InRecovery{}};
  }
}

template<typename S>
auto LeaderStateManager<S>::getStateMachine() const
    -> std::shared_ptr<IReplicatedLeaderState<S>> {
  auto guard = _guardedData.getLockedGuard();
  if (guard->_recoveryCompleted) {
    return guard->_leaderState;
  } else {
    return nullptr;
  }
}

template<typename S>
auto LeaderStateManager<S>::GuardedData::resign() && noexcept
    -> std::pair<std::unique_ptr<CoreType>,
                 std::unique_ptr<replicated_log::IReplicatedLogMethodsBase>> {
  auto core = std::move(*_leaderState).resign();
  // resign the stream after the state, so the state won't try to use the
  // resigned stream.
  auto methods = std::move(*_stream).resign();
  _leaderState = nullptr;
  return {std::move(core), std::move(methods)};
}

}  // namespace arangodb::replication2::replicated_state
