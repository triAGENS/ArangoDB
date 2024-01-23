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

#include "FollowerState.h"

#include <Inspection/VPack.h>

namespace arangodb::replication2::replicated_log {

auto FollowerState::withUpToDate() noexcept -> FollowerState {
  return FollowerState(std::in_place, UpToDate{});
}

auto FollowerState::withErrorBackoff(
    std::chrono::duration<double, std::milli> duration,
    std::size_t retryCount) noexcept -> FollowerState {
  return FollowerState(std::in_place, ErrorBackoff{duration, retryCount});
}

auto FollowerState::withRequestInFlight(
    std::chrono::duration<double, std::milli> duration) noexcept
    -> FollowerState {
  return FollowerState(std::in_place, RequestInFlight{duration});
}

auto FollowerState::fromVelocyPack(velocypack::Slice slice) -> FollowerState {
  auto state = slice.get("state").extract<std::string_view>();
  if (state == static_strings::errorBackoffString) {
    return FollowerState{std::in_place,
                         velocypack::deserialize<ErrorBackoff>(slice)};
  } else if (state == static_strings::requestInFlightString) {
    return FollowerState{std::in_place,
                         velocypack::deserialize<RequestInFlight>(slice)};
  } else {
    return FollowerState{std::in_place,
                         velocypack::deserialize<UpToDate>(slice)};
  }
}

void FollowerState::toVelocyPack(velocypack::Builder& builder) const {
  std::visit([&](auto const& v) { velocypack::serialize(builder, v); }, value);
}

auto to_string(FollowerState const& state) -> std::string_view {
  struct ToStringVisitor {
    auto operator()(FollowerState::UpToDate const&) {
      return static_strings::upToDateString;
    }
    auto operator()(FollowerState::ErrorBackoff const& err) {
      return static_strings::errorBackoffString;
    }
    auto operator()(FollowerState::RequestInFlight const& rif) {
      return static_strings::requestInFlightString;
    }
  };

  return std::visit(ToStringVisitor{}, state.value);
}

}  // namespace arangodb::replication2::replicated_log
