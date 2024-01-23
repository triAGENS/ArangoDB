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

#include <Basics/Result.h>
#include <Basics/ResultT.h>
#include "Replication2/Storage/IPersistor.h"
#include "Replication2/Storage/PersistedStateInfo.h"

namespace arangodb::replication2::storage {

struct IStatePersistor : virtual IPersistor {
  virtual ~IStatePersistor() = default;
  [[nodiscard]] virtual auto updateMetadata(PersistedStateInfo) -> Result = 0;
  [[nodiscard]] virtual auto readMetadata() -> ResultT<PersistedStateInfo> = 0;
};

}  // namespace arangodb::replication2::storage
