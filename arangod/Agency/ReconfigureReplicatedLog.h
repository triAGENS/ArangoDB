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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Job.h"
#include "Supervision.h"
#include "Replication2/ReplicatedLog/LogId.h"

namespace arangodb::consensus {

struct ReconfigureOperation {
  struct AddParticipant {
    ServerID participant;
  };

  struct RemoveParticipant {
    ServerID participant;
  };

  struct SetLeader {
    std::optional<ServerID> participant;
  };

  std::variant<AddParticipant, RemoveParticipant, SetLeader> value;
};

struct ReconfigureReplicatedLog : Job {
  ReconfigureReplicatedLog(Node const& snapshot, AgentInterface* agent,
                           JOB_STATUS status, std::string const& jobId);

  ReconfigureReplicatedLog(
      Node const& snapshot, AgentInterface* agent, std::string const& jobId,
      std::string const& creator, std::string const& database,
      replication2::LogId logId, std::vector<ReconfigureOperation> ops,
      std::optional<ServerID> undoSetLeader = std::nullopt);

  JOB_STATUS status() final;
  bool create(std::shared_ptr<VPackBuilder> envelope = nullptr) final;
  void run(bool&) final;
  bool start(bool&) final;
  Result abort(std::string const& reason) final;
  void addUndoSetLeader(velocypack::Builder& trx);

  DatabaseID _database;
  replication2::LogId _logId;
  std::uint64_t _expectedVersion{0};
  std::vector<ReconfigureOperation> _operations;
  std::optional<ServerID> _undoSetLeader;
};

}  // namespace arangodb::consensus
