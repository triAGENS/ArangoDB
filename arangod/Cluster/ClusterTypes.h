////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Business Source License 1.1 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     https://github.com/arangodb/arangodb/blob/devel/LICENSE
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
#include <limits>
#include <memory>
#include <string>

#include "Basics/Result.h"
#include "Basics/RebootId.h"
#include "Basics/ResourceUsage.h"
#include "Containers/FlatHashMap.h"

namespace arangodb {

typedef std::string ServerID;         // ID of a server
typedef std::string DatabaseID;       // ID/name of a database
typedef std::string CollectionID;     // ID of a collection
typedef uint32_t ServerShortID;       // Short ID of a server
typedef std::string ServerShortName;  // Short name of a server

enum class ServerHealth { kGood, kBad, kFailed, kUnclear };

std::ostream& operator<<(std::ostream& o, ServerHealth r);

struct ServerHealthState {
  RebootId rebootId;
  ServerHealth status;
};

std::ostream& operator<<(std::ostream& o, ServerHealthState const& r);

using ServersKnown = containers::FlatHashMap<ServerID, ServerHealthState>;

struct PeerState {
  std::string serverId;
  RebootId rebootId{0};

  friend auto operator==(PeerState const&, PeerState const&) noexcept
      -> bool = default;

  template<typename Inspector>
  friend auto inspect(Inspector& f, PeerState& x) {
    return f.object(x).fields(f.field("serverId", x.serverId),
                              f.field("rebootId", x.rebootId));
  }
};

auto to_string(PeerState const& peerState) -> std::string;
auto operator<<(std::ostream& ostream, PeerState const& peerState)
    -> std::ostream&;

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack

struct AnalyzersRevision {
 public:
  using Revision = uint64_t;
  using Ptr = std::shared_ptr<AnalyzersRevision const>;

  static constexpr Revision LATEST = std::numeric_limits<uint64_t>::max();
  static constexpr Revision MIN = 0;

  AnalyzersRevision(AnalyzersRevision const&) = delete;
  AnalyzersRevision& operator=(AnalyzersRevision const&) = delete;

  Revision getRevision() const noexcept { return _revision; }

  Revision getBuildingRevision() const noexcept { return _buildingRevision; }

  ServerID const& getServerID() const noexcept { return _serverID; }

  RebootId const& getRebootID() const noexcept { return _rebootID; }

  void toVelocyPack(VPackBuilder& builder) const;

  static Ptr fromVelocyPack(VPackSlice const& slice, std::string& error);

  static Ptr getEmptyRevision();

 private:
  AnalyzersRevision(Revision revision, Revision buildingRevision,
                    ServerID&& serverID, uint64_t rebootID) noexcept
      : _revision(revision),
        _buildingRevision(buildingRevision),
        _serverID(std::move(serverID)),
        _rebootID(rebootID) {}

  Revision _revision;
  Revision _buildingRevision;
  ServerID _serverID;
  RebootId _rebootID;
};

/// @brief Analyzers revisions used in query.
/// Stores current database revision
/// and _system database revision (analyzers from _system are accessible from
/// other databases) If at some point we will decide to allow cross-database
/// anayzer usage this could became more complicated. But for now  we keep it
/// simple - store just two members
struct QueryAnalyzerRevisions {
  constexpr QueryAnalyzerRevisions(AnalyzersRevision::Revision current,
                                   AnalyzersRevision::Revision system)
      : currentDbRevision(current), systemDbRevision(system) {}

  QueryAnalyzerRevisions() = default;
  QueryAnalyzerRevisions(QueryAnalyzerRevisions const&) = default;
  QueryAnalyzerRevisions& operator=(QueryAnalyzerRevisions const&) = default;

  void toVelocyPack(VPackBuilder& builder) const;
  Result fromVelocyPack(velocypack::Slice slice);

  bool isDefault() const noexcept {
    return currentDbRevision == AnalyzersRevision::MIN &&
           systemDbRevision == AnalyzersRevision::MIN;
  }

  bool operator==(QueryAnalyzerRevisions const& other) const noexcept {
    return currentDbRevision == other.currentDbRevision &&
           systemDbRevision == other.systemDbRevision;
  }

  std::ostream& print(std::ostream& o) const;

  bool operator!=(QueryAnalyzerRevisions const& other) const noexcept {
    return !(*this == other);
  }

  /// @brief Gets analyzers revision to be used with specified database
  /// @param vocbase database name
  /// @return analyzers revision
  AnalyzersRevision::Revision getVocbaseRevision(
      std::string_view vocbase) const noexcept;

  static QueryAnalyzerRevisions QUERY_LATEST;

 private:
  AnalyzersRevision::Revision currentDbRevision{AnalyzersRevision::MIN};
  AnalyzersRevision::Revision systemDbRevision{AnalyzersRevision::MIN};
};

std::ostream& operator<<(std::ostream& o,
                         arangodb::QueryAnalyzerRevisions const& r);
}  // namespace arangodb

template<>
struct std::hash<arangodb::PeerState> {
  inline size_t operator()(arangodb::PeerState const& value) const noexcept {
    // TODO Optimize and include rebootId
    return std::hash<std::string>{}(value.serverId);
  }
};
