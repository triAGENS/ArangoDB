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
#include "Replication2/ReplicatedLog/AbstractFollower.h"
#include "Replication2/ReplicatedLog/FollowerState.h"
#include "Replication2/ReplicatedLog/LocalStateMachineStatus.h"
#include "Replication2/ReplicatedLog/LogStatistics.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/QuorumData.h"
#include "Inspection/Transformers.h"

namespace arangodb::velocypack {
class Builder;
class Slice;
}  // namespace arangodb::velocypack

namespace arangodb::replication2::replicated_log {

#if (defined(__GNUC__) && !defined(__clang__))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
struct AppendEntriesErrorReason {
  enum class ErrorType {
    kNone,
    kInvalidLeaderId,
    kLostLogCore,
    kMessageOutdated,
    kWrongTerm,
    kNoPrevLogMatch,
    kPersistenceFailure,
    kCommunicationError,
    kPrevAppendEntriesInFlight,
  };

  ErrorType error = ErrorType::kNone;
  std::optional<std::string> details = std::nullopt;

  [[nodiscard]] auto getErrorMessage() const noexcept -> std::string_view;
  void toVelocyPack(velocypack::Builder& builder) const;
  [[nodiscard]] static auto fromVelocyPack(velocypack::Slice slice)
      -> AppendEntriesErrorReason;
  static auto errorTypeFromString(std::string_view str) -> ErrorType;

  friend auto operator==(AppendEntriesErrorReason const& left,
                         AppendEntriesErrorReason const& right) noexcept
      -> bool = default;
};
#if (defined(__GNUC__) && !defined(__clang__))
#pragma GCC diagnostic pop
#endif

[[nodiscard]] auto to_string(AppendEntriesErrorReason::ErrorType error) noexcept
    -> std::string_view;
struct AppendEntriesErrorReasonTypeStringTransformer {
  using SerializedType = std::string;
  auto toSerialized(AppendEntriesErrorReason::ErrorType source,
                    std::string& target) const -> inspection::Status;
  auto fromSerialized(std::string const& source,
                      AppendEntriesErrorReason::ErrorType& target) const
      -> inspection::Status;
};

template<class Inspector>
auto inspect(Inspector& f, AppendEntriesErrorReason& x) {
  auto state = std::string{static_strings::requestInFlightString};
  auto errorMessage = std::string{x.getErrorMessage()};
  return f.object(x).fields(
      f.field("details", x.details),
      f.field("errorMessage", errorMessage).fallback(std::string{}),
      f.field("error", x.error)
          .transformWith(AppendEntriesErrorReasonTypeStringTransformer{}));
}

}  // namespace arangodb::replication2::replicated_log
