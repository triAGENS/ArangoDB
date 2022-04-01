////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#include "PrototypeFollowerState.h"
#include "PrototypeCore.h"

#include "Logger/LogContextKeys.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_state;
using namespace arangodb::replication2::replicated_state::prototype;

PrototypeFollowerState::PrototypeFollowerState(
    std::unique_ptr<PrototypeCore> core,
    std::shared_ptr<IPrototypeNetworkInterface> networkInterface)
    : loggerContext(core->loggerContext.with<logContextKeyStateComponent>(
          "FollowerState")),
      _logIdentifier(core->getLogId()),
      _networkInterface(std::move(networkInterface)),
      _guardedData(std::move(core)) {}

auto PrototypeFollowerState::acquireSnapshot(ParticipantId const& destination,
                                             LogIndex waitForIndex) noexcept
    -> futures::Future<Result> {
  ResultT<std::shared_ptr<IPrototypeLeaderInterface>> leader =
      _networkInterface->getLeaderInterface(destination);
  if (leader.fail()) {
    return leader.result();
  }
  return leader.get()
      ->getSnapshot(_logIdentifier, waitForIndex)
      .thenValue([self = shared_from_this()](auto&& result) mutable -> Result {
        if (result.fail()) {
          return result.result();
        }

        auto& map = result.get();
        LOG_CTX("85e5a", TRACE, self->loggerContext)
            << " acquired snapshot of size: " << map.size();
        self->_guardedData.doUnderLock(
            [&](auto& core) { core->applySnapshot(map); });
        return TRI_ERROR_NO_ERROR;
      });
}

auto PrototypeFollowerState::applyEntries(
    std::unique_ptr<EntryIterator> ptr) noexcept -> futures::Future<Result> {
  return _guardedData.doUnderLock(
      [self = shared_from_this(), ptr = std::move(ptr)](auto& core) mutable {
        if (!core) {
          return Result{TRI_ERROR_CLUSTER_NOT_FOLLOWER};
        }
        core->applyEntries(std::move(ptr));
        if (core->flush()) {
          auto stream = self->getStream();
          stream->release(core->getLastPersistedIndex());
        }
        return Result{TRI_ERROR_NO_ERROR};
      });
}

auto PrototypeFollowerState::resign() && noexcept
    -> std::unique_ptr<PrototypeCore> {
  return _guardedData.doUnderLock([](auto& core) {
    TRI_ASSERT(core != nullptr);
    return std::move(core);
  });
}

auto PrototypeFollowerState::get(std::string key)
    -> std::optional<std::string> {
  return _guardedData.doUnderLock(
      [key = std::move(key)](auto& core) -> std::optional<std::string> {
        if (!core) {
          return std::nullopt;
        }
        return core->get(key);
      });
}

#include "Replication2/ReplicatedState/ReplicatedState.tpp"