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

#pragma once

#include "Basics/Common.h"
#include "Cluster/ClusterTypes.h"

#include <velocypack/Builder.h>
#include <velocypack/Serializable.h>
#include <velocypack/Slice.h>

#include <atomic>
#include <optional>

namespace arangodb {

enum class ClusterCollectionCreationState { INIT, FAILED, DONE };

struct ClusterCollectionCreationInfo {
  ClusterCollectionCreationInfo(std::string cID, uint64_t shards,
                                uint64_t replicationFactor,
                                uint64_t writeConcern, bool waitForRep,
                                velocypack::Slice const& slice,
                                std::string coordinatorId, RebootId rebootId);

  std::string const collectionID;
  uint64_t numberOfShards;
  uint64_t replicationFactor;
  uint64_t writeConcern;
  bool waitForReplication;
  velocypack::Slice const json;
  std::string name;

  class CreatorInfo : public velocypack::Serializable {
   public:
    CreatorInfo(std::string coordinatorId, RebootId rebootId);

    void toVelocyPack(velocypack::Builder& builder) const override;

    virtual ~CreatorInfo() = default;

    RebootId rebootId() const noexcept;
    std::string const& coordinatorId() const noexcept;

   private:
    std::string _coordinatorId;
    RebootId _rebootId;
  };
  std::optional<CreatorInfo> creator;

 public:
  velocypack::Slice isBuildingSlice() const;

 private:
  bool needsBuildingFlag() const;

 private:
  velocypack::Builder _isBuildingJson;
};
}  // namespace arangodb
