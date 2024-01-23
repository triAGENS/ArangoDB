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

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>
#include <optional>

#include "Basics/StaticStrings.h"
#include "Inspection/Transformers.h"

namespace arangodb::velocypack {
class Builder;
class Slice;
}  // namespace arangodb::velocypack

namespace arangodb::replication2::replicated_log {

namespace static_strings {
inline constexpr std::string_view upToDateString = "up-to-date";
inline constexpr std::string_view errorBackoffString = "error-backoff";
inline constexpr std::string_view requestInFlightString = "request-in-flight";
}  // namespace static_strings

struct FollowerState {
  struct UpToDate {
    friend auto operator==(UpToDate const& left, UpToDate const& right) noexcept
        -> bool = default;
  };
  struct ErrorBackoff {
    std::chrono::duration<double, std::milli> durationMS;
    std::size_t retryCount;
    friend auto operator==(ErrorBackoff const& left,
                           ErrorBackoff const& right) noexcept
        -> bool = default;
  };
  struct RequestInFlight {
    std::chrono::duration<double, std::milli> durationMS;

    friend auto operator==(RequestInFlight const& left,
                           RequestInFlight const& right) noexcept
        -> bool = default;
  };

  std::variant<UpToDate, ErrorBackoff, RequestInFlight> value;

  static auto withUpToDate() noexcept -> FollowerState;
  static auto withErrorBackoff(std::chrono::duration<double, std::milli>,
                               std::size_t retryCount) noexcept
      -> FollowerState;
  static auto withRequestInFlight(
      std::chrono::duration<double, std::milli>) noexcept -> FollowerState;
  static auto fromVelocyPack(velocypack::Slice) -> FollowerState;
  void toVelocyPack(velocypack::Builder&) const;

  FollowerState() = default;

  friend auto operator==(FollowerState const& left,
                         FollowerState const& right) noexcept -> bool = default;

 private:
  template<typename... Args>
  explicit FollowerState(std::in_place_t, Args&&... args)
      : value(std::forward<Args>(args)...) {}
};

template<class Inspector>
auto inspect(Inspector& f, FollowerState::UpToDate& x) {
  auto state = std::string{static_strings::upToDateString};
  return f.object(x).fields(f.field("state", state));
}

template<class Inspector>
auto inspect(Inspector& f, FollowerState::ErrorBackoff& x) {
  auto state = std::string{static_strings::errorBackoffString};
  return f.object(x).fields(
      f.field("state", state),
      f.field("durationMS", x.durationMS)
          .transformWith(inspection::DurationTransformer<
                         std::chrono::duration<double, std::milli>>{}),
      f.field("retryCount", x.retryCount));
}

template<class Inspector>
auto inspect(Inspector& f, FollowerState::RequestInFlight& x) {
  auto state = std::string{static_strings::requestInFlightString};
  return f.object(x).fields(
      f.field("state", state),
      f.field("durationMS", x.durationMS)
          .transformWith(inspection::DurationTransformer<
                         std::chrono::duration<double, std::milli>>{}));
}

template<class Inspector>
auto inspect(Inspector& f, FollowerState& x) {
  if constexpr (Inspector::isLoading) {
    x = FollowerState::fromVelocyPack(f.slice());
  } else {
    x.toVelocyPack(f.builder());
  }
  return arangodb::inspection::Status::Success{};
}

auto to_string(FollowerState const&) -> std::string_view;

}  // namespace arangodb::replication2::replicated_log
