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

#include "ReplicatedStateManager.h"

#include "Basics/DownCast.h"
#include "Logger/LogContextKeys.h"
#include "Replication2/Storage/PersistedStateInfo.h"

namespace arangodb::replication2::replicated_state {

template<typename S>
ReplicatedStateManager<S>::ReplicatedStateManager(
    LoggerContext loggerContext,
    std::shared_ptr<ReplicatedStateMetrics> metrics,
    std::unique_ptr<CoreType> logCore, std::shared_ptr<Factory> factory,
    std::shared_ptr<IScheduler> scheduler)
    : _loggerContext(std::move(loggerContext)),
      _metrics(std::move(metrics)),
      _factory(std::move(factory)),
      _scheduler(std::move(scheduler)),
      _guarded{std::make_shared<UnconfiguredStateManager<S>>(
          _loggerContext.with<logContextKeyStateRole>(
              static_strings::StringUnconfigured),
          std::move(logCore))} {}

template<typename S>
void ReplicatedStateManager<S>::acquireSnapshot(ServerID leader,
                                                LogIndex commitIndex,
                                                std::uint64_t version) {
  auto guard = _guarded.getLockedGuard();

  std::visit(overload{
                 [&](std::shared_ptr<FollowerStateManager<S>>& manager) {
                   LOG_CTX("52a11", DEBUG, _loggerContext)
                       << "try to acquire a new snapshot, starting at "
                       << commitIndex;
                   manager->acquireSnapshot(leader, commitIndex, version);
                 },
                 [](auto&) {
                   ADB_PROD_ASSERT(false)
                       << "State is not a follower (or uninitialized), but "
                          "acquireSnapshot is called";
                 },
             },
             guard->_currentManager);
}

template<typename S>
void ReplicatedStateManager<S>::updateCommitIndex(LogIndex index) {
  auto guard = _guarded.getLockedGuard();

  std::visit(overload{
                 [index](auto& manager) { manager->updateCommitIndex(index); },
                 [](std::shared_ptr<UnconfiguredStateManager<S>>& manager) {
                   ADB_PROD_ASSERT(false) << "update commit index called on "
                                             "an unconfigured state manager";
                 },
             },
             guard->_currentManager);
}

template<typename S>
auto ReplicatedStateManager<S>::resignCurrentState() noexcept
    -> std::unique_ptr<replicated_log::IReplicatedLogMethodsBase> {
  auto guard = _guarded.getLockedGuard();
  auto&& [core, methods] =
      std::visit([](auto& manager) { return std::move(*manager).resign(); },
                 guard->_currentManager);
  // TODO Is it allowed to happen that resign() is called on an unconfigured
  //      state?
  ADB_PROD_ASSERT(
      std::holds_alternative<std::shared_ptr<UnconfiguredStateManager<S>>>(
          guard->_currentManager) == (methods == nullptr));
  guard->_currentManager
      .template emplace<std::shared_ptr<UnconfiguredStateManager<S>>>(
          std::make_shared<UnconfiguredStateManager<S>>(
              _loggerContext.template with<logContextKeyStateRole>(
                  static_strings::StringUnconfigured),
              std::move(core)));
  return std::move(methods);
}

template<typename S>
void ReplicatedStateManager<S>::leadershipEstablished(
    std::unique_ptr<replicated_log::IReplicatedLogLeaderMethods> methods) {
  auto guard = _guarded.getLockedGuard();
  ADB_PROD_ASSERT(
      std::holds_alternative<std::shared_ptr<UnconfiguredStateManager<S>>>(
          guard->_currentManager));
  auto&& [core, oldMethods] =
      std::move(*std::get<std::shared_ptr<UnconfiguredStateManager<S>>>(
                    guard->_currentManager))
          .resign();
  ADB_PROD_ASSERT(oldMethods == nullptr);
  auto stream = std::make_shared<typename LeaderStateManager<S>::StreamImpl>(
      std::move(methods));
  auto leaderState = _factory->constructLeader(std::move(core), stream);
  auto& manager =
      guard->_currentManager
          .template emplace<std::shared_ptr<LeaderStateManager<S>>>(
              std::make_shared<LeaderStateManager<S>>(
                  _loggerContext.template with<logContextKeyStateRole>(
                      static_strings::StringLeader),
                  _metrics, std::move(leaderState), std::move(stream)));

  // must be posted on the scheduler to avoid deadlocks with the log
  _scheduler->queue([weak = manager->weak_from_this()]() mutable {
    if (auto manager = weak.lock(); manager != nullptr) {
      manager->recoverEntries();
    }
  });
}

template<typename S>
void ReplicatedStateManager<S>::becomeFollower(
    std::unique_ptr<replicated_log::IReplicatedLogFollowerMethods> methods) {
  auto guard = _guarded.getLockedGuard();
  ADB_PROD_ASSERT(
      std::holds_alternative<std::shared_ptr<UnconfiguredStateManager<S>>>(
          guard->_currentManager));
  auto&& [core, oldMethods] =
      std::move(*std::get<std::shared_ptr<UnconfiguredStateManager<S>>>(
                    guard->_currentManager))
          .resign();
  ADB_PROD_ASSERT(oldMethods == nullptr);

  auto stream = std::make_shared<typename FollowerStateManager<S>::StreamImpl>(
      std::move(methods));
  auto followerState =
      _factory->constructFollower(std::move(core), stream, _scheduler);
  auto stateManager = std::make_shared<FollowerStateManager<S>>(
      _loggerContext.template with<logContextKeyStateRole>(
          static_strings::StringFollower),
      _metrics, followerState, std::move(stream), _scheduler);
  followerState->setStateManager(stateManager);
  guard->_currentManager
      .template emplace<std::shared_ptr<FollowerStateManager<S>>>(
          std::move(stateManager));
}

template<typename S>
auto ReplicatedStateManager<S>::getInternalStatus() const -> Status {
  auto manager = _guarded.getLockedGuard()->_currentManager;
  auto status = std::visit(overload{[](auto const& manager) -> Status {
                             return {manager->getInternalStatus()};
                           }},
                           manager);

  return status;
}

template<typename S>
auto ReplicatedStateManager<S>::getFollower() const
    -> std::shared_ptr<IReplicatedFollowerStateBase> {
  auto guard = _guarded.getLockedGuard();
  return std::visit(
      overload{
          [](std::shared_ptr<FollowerStateManager<S>> const& manager) {
            return basics::downCast<IReplicatedFollowerStateBase>(
                manager->getStateMachine());
          },
          [](auto const&) -> std::shared_ptr<IReplicatedFollowerStateBase> {
            return nullptr;
          }},
      guard->_currentManager);
}

template<typename S>
auto ReplicatedStateManager<S>::getLeader() const
    -> std::shared_ptr<IReplicatedLeaderStateBase> {
  auto guard = _guarded.getLockedGuard();
  return std::visit(
      overload{[](std::shared_ptr<LeaderStateManager<S>> const& manager) {
                 return basics::downCast<IReplicatedLeaderStateBase>(
                     manager->getStateMachine());
               },
               [](auto const&) -> std::shared_ptr<IReplicatedLeaderStateBase> {
                 return nullptr;
               }},
      guard->_currentManager);
}

template<typename S>
auto ReplicatedStateManager<S>::resign() && -> std::unique_ptr<CoreType> {
  auto guard = _guarded.getLockedGuard();
  auto [core, methods] =
      std::visit([](auto&& mgr) { return std::move(*mgr).resign(); },
                 guard->_currentManager);
  // we should be unconfigured already
  TRI_ASSERT(methods == nullptr);
  // cppcheck-suppress returnStdMoveLocal
  return std::move(core);
}

}  // namespace arangodb::replication2::replicated_state
