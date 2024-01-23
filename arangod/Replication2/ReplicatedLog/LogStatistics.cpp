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

#include "LogStatistics.h"

#include <Basics/Exceptions.h>
#include <Basics/application-exit.h>
#include <Basics/voc-errors.h>
#include <Inspection/VPack.h>
#include <Logger/LogMacros.h>
#include <velocypack/Builder.h>
#include <velocypack/Iterator.h>

#include <cstddef>
#include <functional>
#include <utility>

namespace arangodb::replication2::replicated_log {

void replicated_log::LogStatistics::toVelocyPack(
    velocypack::Builder& builder) const {
  serialize(builder, *this);
}

auto replicated_log::LogStatistics::fromVelocyPack(velocypack::Slice slice)
    -> LogStatistics {
  return deserialize<LogStatistics>(slice);
}

}  // namespace arangodb::replication2::replicated_log
