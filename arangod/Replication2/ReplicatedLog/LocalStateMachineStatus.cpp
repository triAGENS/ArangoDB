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

#include "LocalStateMachineStatus.h"

#include <ostream>
#include <Assertions/Assert.h>
#include <Logger/LogMacros.h>

namespace arangodb::replication2::replicated_log {

auto to_string(LocalStateMachineStatus status) noexcept -> std::string_view {
  switch (status) {
    case LocalStateMachineStatus::kUnconfigured:
      return kStringUnconfigured;
    case LocalStateMachineStatus::kConnecting:
      return kStringConnecting;
    case LocalStateMachineStatus::kRecovery:
      return kStringRecovery;
    case LocalStateMachineStatus::kAcquiringSnapshot:
      return kStringAcquiringSnapshot;
    case LocalStateMachineStatus::kOperational:
      return kStringOperational;
  }
  LOG_TOPIC("e3242", ERR, Logger::REPLICATION2)
      << "Unhandled replicated state status: "
      << static_cast<std::underlying_type_t<decltype(status)>>(status);
  TRI_ASSERT(false);
  return "(unknown status code)";
}

auto operator<<(std::ostream& ostream, LocalStateMachineStatus const& status)
    -> std::ostream& {
  return ostream << to_string(status);
}

}  // namespace arangodb::replication2::replicated_log
