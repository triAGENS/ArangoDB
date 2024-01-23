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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Replication2/ReplicatedLog/Components/IStateHandleManager.h"
#include "Basics/Guarded.h"
#include "Replication2/ReplicatedState/StateStatus.h"

namespace arangodb::replication2::replicated_log {
inline namespace comp {
struct IFollowerCommitManager;

struct StateHandleManager : IStateHandleManager {
  explicit StateHandleManager(
      std::unique_ptr<IReplicatedStateHandle> stateHandle,
      IFollowerCommitManager& commit);

  auto resign() noexcept -> std::unique_ptr<IReplicatedStateHandle> override;
  auto updateCommitIndex(LogIndex index, bool snapshotAvailable) noexcept
      -> DeferredAction override;
  void becomeFollower(
      std::unique_ptr<IReplicatedLogFollowerMethods> ptr) override;

  void acquireSnapshot(ParticipantId const& leader,
                       std::uint64_t version) noexcept override;
  auto getInternalStatus() const -> replicated_state::Status;

 private:
  struct GuardedData {
    explicit GuardedData(std::unique_ptr<IReplicatedStateHandle> stateHandlePtr,
                         IFollowerCommitManager& commit);
    std::unique_ptr<IReplicatedStateHandle> stateHandle;
    IFollowerCommitManager& commit;
  };

  Guarded<GuardedData> guardedData;
};
}  // namespace comp
}  // namespace arangodb::replication2::replicated_log
