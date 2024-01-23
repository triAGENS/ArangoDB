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

#include <iosfwd>
#include <string_view>

namespace arangodb::replication2::replicated_log {

constexpr inline std::string_view kStringUnconfigured = "Unconfigured";
constexpr inline std::string_view kStringConnecting = "Connecting";
constexpr inline std::string_view kStringRecovery = "RecoveryInProgress";
constexpr inline std::string_view kStringAcquiringSnapshot =
    "AcquiringSnapshot";
constexpr inline std::string_view kStringOperational = "ServiceOperational";

enum class LocalStateMachineStatus {
  // resigned or not constructed
  kUnconfigured,
  // a follower is connecting before it processed its first append entries
  // request successfully
  kConnecting,
  // a leader is in this state until it has completed recovery
  kRecovery,
  // a follower that has established a connection to the leader, but doesn't
  // have a snapshot yet
  kAcquiringSnapshot,
  // state machine is operational, i.e. on a leader, recovery has completed
  // succesfully; and on a follower, it has established a connection to the
  // leader (received and processed an append entries request successfully) and
  // has a valid snapshot
  kOperational
};

auto to_string(LocalStateMachineStatus) noexcept -> std::string_view;
auto operator<<(std::ostream& ostream, LocalStateMachineStatus const& status)
    -> std::ostream&;

template<class Inspector>
auto inspect(Inspector& f, LocalStateMachineStatus& x) {
  return f.enumeration(x).values(
      LocalStateMachineStatus::kUnconfigured, kStringUnconfigured,
      LocalStateMachineStatus::kConnecting, kStringConnecting,
      LocalStateMachineStatus::kRecovery, kStringRecovery,
      LocalStateMachineStatus::kAcquiringSnapshot, kStringAcquiringSnapshot,
      LocalStateMachineStatus::kOperational, kStringOperational);
}

}  // namespace arangodb::replication2::replicated_log
