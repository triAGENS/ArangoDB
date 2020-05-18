////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGOD_CLUSTER_CLUSTERTYPES_H
#define ARANGOD_CLUSTER_CLUSTERTYPES_H

#include <limits>
#include <string>
#include <iostream>
#include <memory>
#include "velocypack/Builder.h"
#include "velocypack/velocypack-aliases.h"

namespace arangodb {

typedef std::string ServerID;         // ID of a server
typedef std::string DatabaseID;       // ID/name of a database
typedef std::string CollectionID;     // ID of a collection
typedef std::string ViewID;           // ID of a view
typedef std::string ShardID;          // ID of a shard
typedef uint32_t ServerShortID;       // Short ID of a server
typedef std::string ServerShortName;  // Short name of a server

class RebootId {
 public:
  explicit constexpr RebootId(uint64_t rebootId) noexcept : _value(rebootId) {}
  uint64_t value() const noexcept { return _value; }

  bool initialized() const noexcept { return value() != 0; }

  bool operator==(RebootId other) const noexcept {
    return value() == other.value();
  }
  bool operator!=(RebootId other) const noexcept {
    return value() != other.value();
  }
  bool operator<(RebootId other) const noexcept {
    return value() < other.value();
  }
  bool operator>(RebootId other) const noexcept {
    return value() > other.value();
  }
  bool operator<=(RebootId other) const noexcept {
    return value() <= other.value();
  }
  bool operator>=(RebootId other) const noexcept {
    return value() >= other.value();
  }

  static constexpr RebootId max() noexcept {
    return RebootId{std::numeric_limits<decltype(_value)>::max()};
  }

  std::ostream& print(std::ostream& o) const {
    o << _value;
    return o;
  }

 private:
  uint64_t _value;
};

namespace velocypack {
class Builder;
class Slice;
}

struct AnalyzersRevision {
 public:
  using Revision = uint64_t;
  using Ptr = std::shared_ptr<AnalyzersRevision const>;

  static constexpr Revision LATEST = std::numeric_limits<uint64_t>::max();
  static constexpr Revision MIN = 0;

  struct Key { // passkey idiom
   private:
    friend struct AnalyzersRevision;
    Key() {}
    Key(Key const&) = default;
  };

  AnalyzersRevision(Revision revision, Revision buildingRevision,
    ServerID&& serverID, uint64_t rebootID, Key&) noexcept
    : _revision(revision), _buildingRevision(buildingRevision),
    _serverID(std::move(serverID)), _rebootID(rebootID) {}

  AnalyzersRevision(AnalyzersRevision const&) = delete;
  AnalyzersRevision& operator=(AnalyzersRevision const&) = delete;

  Revision getRevision() const noexcept {
    return _revision;
  }

  Revision getBuildingRevision() const noexcept {
    return _buildingRevision;
  }

  ServerID const& getServerID() const noexcept {
    return _serverID;
  }

  RebootId const& getRebootID() const noexcept {
    return _rebootID;
  }

  void toVelocyPack(VPackBuilder& builder) const;

  static Ptr fromVelocyPack(VPackSlice const& slice, std::string& error);

  static Ptr getEmptyRevision();
 private:
  Revision _revision;
  Revision _buildingRevision;
  ServerID _serverID;
  RebootId _rebootID;
};

}  // namespace arangodb

std::ostream& operator<< (std::ostream& o, arangodb::RebootId const& r);

#endif  // ARANGOD_CLUSTER_CLUSTERTYPES_H
