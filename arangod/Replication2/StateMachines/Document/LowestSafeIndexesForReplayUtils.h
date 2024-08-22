////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
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

#include "LowestSafeIndexesForReplay.h"

#include "Replication2/Streams/Streams.h"
#include "Replication2/StateMachines/Document/DocumentStateMachine.h"

namespace arangodb::replication2::replicated_state::document {

void increaseAndPersistLowestSafeIndexForReplayTo(
    LoggerContext loggerContext,
    LowestSafeIndexesForReplay& lowestSafeIndexesForReplay,
    streams::Stream<DocumentState>& stream, ShardID shardId, LogIndex logIndex);

}
