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

#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"

#include "Inspection/Transformers.h"

namespace arangodb::replication2 {

struct LogMetaPayload {
  struct FirstEntryOfTerm {
    ParticipantId leader;
    agency::ParticipantsConfig participants;

    friend auto operator==(FirstEntryOfTerm const&,
                           FirstEntryOfTerm const&) noexcept -> bool = default;

    template<typename Inspector>
    friend auto inspect(Inspector& f, FirstEntryOfTerm& x) {
      return f.object(x).fields(f.field("leader", x.leader),
                                f.field("participants", x.participants));
    }
  };

  static auto withFirstEntryOfTerm(ParticipantId leader,
                                   agency::ParticipantsConfig config)
      -> LogMetaPayload;

  struct UpdateInnerTermConfig {
    agency::ParticipantsConfig participants;
    std::unordered_map<ParticipantId, RebootId> safeRebootIds;

    friend auto operator==(UpdateInnerTermConfig const&,
                           UpdateInnerTermConfig const&) noexcept
        -> bool = default;
    template<typename Inspector>
    friend auto inspect(Inspector& f, UpdateInnerTermConfig& x) {
      return f.object(x).fields(f.field("participants", x.participants),
                                f.field("safeRebootIds", x.safeRebootIds));
    }
  };

  static auto withUpdateInnerTermConfig(
      agency::ParticipantsConfig config,
      std::unordered_map<ParticipantId, RebootId> safeRebootIds)
      -> LogMetaPayload;

  struct Ping {
    using clock = std::chrono::system_clock;
    std::optional<std::string> message;
    clock::time_point time;

    friend auto operator==(Ping const&, Ping const&) noexcept -> bool = default;
    template<typename Inspector>
    friend auto inspect(Inspector& f, Ping& x) {
      return f.object(x).fields(
          f.field("message", x.message),
          f.field("time", x.time)
              .transformWith(inspection::TimeStampTransformer{}));
    }
  };

  static auto withPing(std::optional<std::string> message,
                       Ping::clock::time_point = Ping::clock::now()) noexcept
      -> LogMetaPayload;

  static auto fromVelocyPack(velocypack::Slice) -> LogMetaPayload;
  void toVelocyPack(velocypack::Builder&) const;

  std::variant<FirstEntryOfTerm, UpdateInnerTermConfig, Ping> info;

  template<typename Inspector>
  friend auto inspect(Inspector& f, LogMetaPayload& x) {
    namespace insp = arangodb::inspection;
    return f.variant(x.info).embedded("type").alternatives(
        insp::type<FirstEntryOfTerm>("FirstEntryOfTerm"),
        insp::type<UpdateInnerTermConfig>("UpdateInnerTermConfig"),
        insp::type<Ping>("Ping"));
  }

  friend auto operator==(LogMetaPayload const&, LogMetaPayload const&) noexcept
      -> bool = default;
};

}  // namespace arangodb::replication2
