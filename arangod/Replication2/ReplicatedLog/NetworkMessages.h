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

#include <Basics/ErrorCode.h>
#include <Containers/ImmerMemoryPolicy.h>

#include <string>
#include <ostream>

#if (_MSC_VER >= 1)
// suppress warnings:
#pragma warning(push)
// conversion from 'size_t' to 'immer::detail::rbts::count_t', possible loss of
// data
#pragma warning(disable : 4267)
// result of 32-bit shift implicitly converted to 64 bits (was 64-bit shift
// intended?)
#pragma warning(disable : 4334)
#endif
#include <immer/flex_vector.hpp>
#if (_MSC_VER >= 1)
#pragma warning(pop)
#endif

#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/InMemoryLogEntry.h"
#include "Replication2/ReplicatedLog/AppendEntriesErrorReason.h"

namespace arangodb {
class Result;
}

namespace arangodb::replication2::replicated_log {

struct MessageId {
  constexpr MessageId() noexcept : _value{0} {}
  constexpr explicit MessageId(std::uint64_t value) noexcept : _value{value} {}

  friend auto operator<=>(MessageId, MessageId) noexcept = default;
  friend auto operator++(MessageId& id) -> MessageId&;
  friend auto operator<<(std::ostream& os, MessageId id) -> std::ostream&;
  friend auto to_string(MessageId id) -> std::string;

  [[nodiscard]] explicit operator velocypack::Value() const noexcept;

  template<class Inspector>
  friend auto inspect(Inspector&, MessageId&);
  friend struct fmt::formatter<MessageId>;

 protected:
  auto value() -> std::uint64_t;

 private:
  std::uint64_t _value;
};

auto operator++(MessageId& id) -> MessageId&;
auto operator<<(std::ostream& os, MessageId id) -> std::ostream&;
auto to_string(MessageId id) -> std::string;

template<class Inspector>
auto inspect(Inspector& f, MessageId& x) {
  if constexpr (Inspector::isLoading) {
    auto v = std::uint64_t{};
    auto res = f.apply(v);
    if (res.ok()) {
      x = MessageId{v};
    }
    return res;
  } else {
    return f.apply(x.value());
  }
}

#if (defined(__GNUC__) && !defined(__clang__))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
struct AppendEntriesResult {
  LogTerm logTerm;
  ErrorCode errorCode;
  AppendEntriesErrorReason reason;
  MessageId messageId;
  // TODO With some error reasons (at least kLostLogCore, i.e. when the follower
  //      resigned already) information about the snapshot is unavailable. Maybe
  //      this should be an optional<bool>.
  bool snapshotAvailable{false};

  LogIndex syncIndex;

  std::optional<TermIndexPair> conflict;

  [[nodiscard]] auto isSuccess() const noexcept -> bool;

  AppendEntriesResult(LogTerm term, MessageId id, TermIndexPair conflict,
                      AppendEntriesErrorReason reason, bool snapshotAvailable,
                      LogIndex syncIndex) noexcept;
  AppendEntriesResult(LogTerm, MessageId, bool snapshotAvailable,
                      LogIndex syncIndex) noexcept;
  AppendEntriesResult(LogTerm logTerm, ErrorCode errorCode,
                      AppendEntriesErrorReason reason, MessageId,
                      bool snapshotAvailable, LogIndex syncIndex) noexcept;
  void toVelocyPack(velocypack::Builder& builder) const;
  static auto fromVelocyPack(velocypack::Slice slice) -> AppendEntriesResult;

  static auto withConflict(LogTerm, MessageId, TermIndexPair conflict,
                           bool snapshotAvailable, LogIndex syncIndex) noexcept
      -> AppendEntriesResult;
  static auto withRejection(LogTerm, MessageId, AppendEntriesErrorReason,
                            bool snapshotAvailable, LogIndex syncIndex) noexcept
      -> AppendEntriesResult;
  static auto withPersistenceError(LogTerm, MessageId, Result const&,
                                   bool snapshotAvailable,
                                   LogIndex syncIndex) noexcept
      -> AppendEntriesResult;
  static auto withOk(LogTerm, MessageId, bool snapshotAvailable,
                     LogIndex syncIndex) noexcept -> AppendEntriesResult;
};
#if (defined(__GNUC__) && !defined(__clang__))
#pragma GCC diagnostic pop
#endif

auto to_string(AppendEntriesResult const& res) -> std::string;

struct AppendEntriesRequest {
  using EntryContainer =
      ::immer::flex_vector<InMemoryLogEntry,
                           arangodb::immer::arango_memory_policy>;

  LogTerm leaderTerm;
  ParticipantId leaderId;
  TermIndexPair prevLogEntry;
  LogIndex leaderCommit;
  LogIndex lowestIndexToKeep;
  MessageId messageId;
  EntryContainer entries{};
  bool waitForSync = false;

  AppendEntriesRequest() = default;
  AppendEntriesRequest(LogTerm leaderTerm, ParticipantId leaderId,
                       TermIndexPair prevLogEntry, LogIndex leaderCommit,
                       LogIndex lowestIndexToKeep, MessageId messageId,
                       bool waitForSync, EntryContainer entries);
  ~AppendEntriesRequest() noexcept = default;

  AppendEntriesRequest(AppendEntriesRequest&& other) noexcept;
  AppendEntriesRequest(AppendEntriesRequest const& other) = default;
  auto operator=(AppendEntriesRequest&& other) noexcept
      -> AppendEntriesRequest&;
  auto operator=(AppendEntriesRequest const& other)
      -> AppendEntriesRequest& = default;

  void toVelocyPack(velocypack::Builder& builder) const;
  static auto fromVelocyPack(velocypack::Slice slice) -> AppendEntriesRequest;
};

struct SnapshotAvailableReport {
  /// Last message id received from the leader. This is reported to
  /// the leader, so it can ignore snapshot status updates from
  /// append entries responses that are lower than or equal to this
  /// id, as they are less recent than this information.
  MessageId messageId;
};

template<class Inspector>
auto inspect(Inspector& f, SnapshotAvailableReport& x) {
  using namespace arangodb;
  return f.object(x).fields(f.field(StaticStrings::MessageId, x.messageId));
}

}  // namespace arangodb::replication2::replicated_log

namespace arangodb {
template<>
struct velocypack::Extractor<replication2::replicated_log::MessageId> {
  static auto extract(velocypack::Slice slice)
      -> replication2::replicated_log::MessageId {
    return replication2::replicated_log::MessageId{
        slice.getNumericValue<uint64_t>()};
  }
};
}  // namespace arangodb

template<>
struct fmt::formatter<::arangodb::replication2::replicated_log::MessageId>
    : fmt::formatter<std::uint64_t> {
  template<class FormatContext>
  auto format(::arangodb::replication2::replicated_log::MessageId mid,
              FormatContext& fc) const {
    return ::fmt::formatter<typename std::uint64_t>::format(mid.value(), fc);
  }
  template<typename ParseContext>
  auto parse(ParseContext& ctx) {
    return ::fmt::formatter<typename std::uint64_t>::parse(ctx);
  }
};
