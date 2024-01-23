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

#pragma once

#include "Replication2/ReplicatedLog/LogId.h"
#include "Replication2/ReplicatedLog/ParticipantId.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Cluster/ClusterTypes.h"

namespace arangodb::network {
class ConnectionPool;
}

namespace arangodb::replication2::replicated_log {

struct NetworkAttachedFollower : AbstractFollower {
  explicit NetworkAttachedFollower(
      network::ConnectionPool* pool, ParticipantId id, DatabaseID database,
      LogId logId, std::shared_ptr<ReplicatedLogGlobalSettings const> options);
  [[nodiscard]] auto getParticipantId() const noexcept
      -> ParticipantId const& override;
  auto appendEntries(AppendEntriesRequest request)
      -> futures::Future<AppendEntriesResult> override;

 private:
  network::ConnectionPool* pool;
  ParticipantId id;
  DatabaseID database;
  LogId logId;
  std::shared_ptr<ReplicatedLogGlobalSettings const> options;
};

struct NetworkLeaderCommunicator : ILeaderCommunicator {
  explicit NetworkLeaderCommunicator(network::ConnectionPool* pool,
                                     ParticipantId leader, DatabaseID database,
                                     LogId logId);
  auto getParticipantId() const noexcept -> ParticipantId const& override;
  auto reportSnapshotAvailable(MessageId) noexcept
      -> futures::Future<Result> override;

 private:
  network::ConnectionPool* pool;
  ParticipantId leader;
  DatabaseID database;
  LogId logId;
};

}  // namespace arangodb::replication2::replicated_log
