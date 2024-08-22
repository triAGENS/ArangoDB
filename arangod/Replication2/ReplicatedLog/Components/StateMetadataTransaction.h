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

#include "IStateMetadataTransaction.h"
#include "IStorageManager.h"

namespace arangodb::replication2::replicated_log {
struct StateMetadataTransaction : IStateMetadataTransaction {
  explicit StateMetadataTransaction(std::unique_ptr<IStateInfoTransaction> trx);
  auto get() noexcept -> DataType& override;

  std::unique_ptr<IStateInfoTransaction> trx;
};
}  // namespace arangodb::replication2::replicated_log
