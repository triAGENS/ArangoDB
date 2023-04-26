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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "ClusterCollectionCreationInfo.h"

#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterTypes.h"

#include <velocypack/Collection.h>
#include <utility>

using namespace arangodb;

ClusterCollectionCreationInfo::ClusterCollectionCreationInfo(
    std::string cID, uint64_t shards, uint64_t replicationFactor,
    uint64_t writeConcern, bool waitForRep, velocypack::Slice const& slice,
    std::string coordinatorId, RebootId rebootId)
    : collectionID(std::move(cID)),
      numberOfShards(shards),
      replicationFactor(replicationFactor),
      writeConcern(writeConcern),
      waitForReplication(waitForRep),
      json(slice),
      name(basics::VelocyPackHelper::getStringValue(
          json, StaticStrings::DataSourceName, StaticStrings::Empty)),
      creator(std::in_place, std::move(coordinatorId), rebootId) {
  TRI_ASSERT(creator);
  TRI_ASSERT(creator->rebootId().initialized());
  TRI_ASSERT(!name.empty());
  if (needsBuildingFlag()) {
    VPackBuilder tmp;
    tmp.openObject();
    tmp.add(StaticStrings::AttrIsBuilding, VPackValue(true));
    if (creator) {
      creator->toVelocyPack(tmp);
    }
    tmp.close();
    _isBuildingJson = VPackCollection::merge(json, tmp.slice(), true, false);
  }
}

VPackSlice ClusterCollectionCreationInfo::isBuildingSlice() const {
  if (needsBuildingFlag()) {
    return _isBuildingJson.slice();
  }
  return json;
}

bool ClusterCollectionCreationInfo::needsBuildingFlag() const {
  // Deactivated the SmartGraph check, our testing mock for coordinator side
  // tries to get away without other servers by initially adding only 0
  // shard collections (non-smart). We do not want to loose these test.
  // So we will loose the more precise check for now.
  /*
  return numberOfShards > 0 ||
         basics::VelocyPackHelper::getBooleanValue(json, StaticStrings::IsSmart,
  false);
  */
  return numberOfShards > 0;
}

void ClusterCollectionCreationInfo::CreatorInfo::toVelocyPack(
    velocypack::Builder& builder) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add(StaticStrings::AttrCoordinator, VPackValue(coordinatorId()));
  builder.add(StaticStrings::AttrCoordinatorRebootId,
              VPackValue(rebootId().value()));
}

ClusterCollectionCreationInfo::CreatorInfo::CreatorInfo(
    std::string coordinatorId, RebootId rebootId)
    : _coordinatorId(std::move(coordinatorId)), _rebootId(rebootId) {}

RebootId ClusterCollectionCreationInfo::CreatorInfo::rebootId() const noexcept {
  return _rebootId;
}

std::string const& ClusterCollectionCreationInfo::CreatorInfo::coordinatorId()
    const noexcept {
  return _coordinatorId;
}
