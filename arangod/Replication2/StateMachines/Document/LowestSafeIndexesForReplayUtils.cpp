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

#include "LowestSafeIndexesForReplayUtils.h"

#include "Basics/MaintainerMode.h"

namespace arangodb::replication2::replicated_state::document {

namespace {
// used only in maintainer mode
[[maybe_unused]] bool lsfifrMapsAreEqual(
    std::map<ShardID, LogIndex> left_, std::map<std::string, LogIndex> right) {
  auto left = std::map<std::string, LogIndex>{};
  std::transform(left_.begin(), left_.end(), std::inserter(left, left.end()),
                 [](auto const& kv) {
                   return std::pair{ShardID(kv.first), kv.second};
                 });
  return left == right;
}
}  // namespace

void increaseAndPersistLowestSafeIndexForReplayTo(
    LoggerContext loggerContext,
    LowestSafeIndexesForReplay& lowestSafeIndexesForReplay,
    streams::Stream<DocumentState>& stream, ShardID shardId,
    LogIndex logIndex) {
  auto trx = stream.beginMetadataTrx();
  auto& metadata = trx->get();
  if constexpr (maintainerMode) {
    auto const& inMemMap = lowestSafeIndexesForReplay.getMap();
    auto persistedMap = metadata.lowestSafeIndexesForReplay;
    if (!lsfifrMapsAreEqual(inMemMap, persistedMap)) {
      auto msg = std::stringstream{};
      msg << "Mismatch between in-memory and persisted state of lowest safe "
             "indexes for replay. In-memory state: "
          << inMemMap << ", persisted state: ";
      // no ADL for persistedMap
      arangodb::operator<<(msg, persistedMap);
      TRI_ASSERT(false) << msg.str();
    }
  }
  auto& lowestSafeIndex =
      metadata.lowestSafeIndexesForReplay[shardId.operator std::string()];
  lowestSafeIndex = std::max(lowestSafeIndex, logIndex);
  auto const res = stream.commitMetadataTrx(std::move(trx));
  if (res.ok()) {
    lowestSafeIndexesForReplay.setFromMetadata(metadata);
  } else {
    auto msg = fmt::format(
        "Failed to persist the lowest safe index on shard {}. This "
        "will abort index creation. Error was: {}",
        shardId, res.errorMessage());
    LOG_CTX("9afad", WARN, loggerContext) << msg;
    THROW_ARANGO_EXCEPTION_MESSAGE(res.errorNumber(), std::move(msg));
  }
}
}  // namespace arangodb::replication2::replicated_state::document
