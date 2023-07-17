////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2023-2023 ArangoDB GmbH, Cologne, Germany
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

#include "Replication2/ReplicatedLog/ReplicatedLog.h"

struct TRI_vocbase_t;

namespace arangodb::replication2::test {

struct FakeFollowerFactory : replicated_log::IAbstractFollowerFactory {
  FakeFollowerFactory(TRI_vocbase_t& vocbase, replication2::LogId id)
      : vocbase(vocbase), id(id) {}
  auto constructFollower(const ParticipantId& participantId) -> std::shared_ptr<
      replication2::replicated_log::AbstractFollower> override {
    return nullptr;
  }

  auto constructLeaderCommunicator(ParticipantId const& participantId)
      -> std::shared_ptr<replicated_log::ILeaderCommunicator> override {
    return leaderComm;
  }

  TRI_vocbase_t& vocbase;
  replication2::LogId id;
  std::shared_ptr<replicated_log::ILeaderCommunicator> leaderComm;
};
}  // namespace arangodb::replication2::test