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

#include <unordered_map>

#include "Basics/Identifier.h"
#include "Cluster/ClusterTypes.h"
#include "Cluster/Utils/PlanShardToServerMappping.h"
#include "Inspection/Access.h"
#include "VocBase/voc-types.h"
#include "VocBase/Properties/KeyGeneratorProperties.h"
#include "VocBase/Identifiers/DataSourceId.h"
#include "VocBase/Properties/CollectionInternalProperties.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "VocBase/Properties/CollectionIndexesProperties.h"

namespace arangodb::replication2::agency {

struct CollectionGroupId : basics::Identifier {
  using Identifier::Identifier;
};

/***
 * SECTION Collection Groups
 */
struct CollectionGroup {
  CollectionGroupId id;
  CollectionID groupLeader;

  struct Collection {};
  std::unordered_map<CollectionID, Collection> collections;

  struct Attributes {
    struct MutableAttributes {
      std::size_t writeConcern{};
      std::size_t replicationFactor{};
      bool waitForSync{};

      bool operator==(MutableAttributes const& other) const noexcept = default;
    };

    MutableAttributes mutableAttributes;

    struct ImmutableAttributes {
      std::size_t numberOfShards;
    };

    ImmutableAttributes immutableAttributes;
  };
  Attributes attributes;
};

struct CollectionGroupTargetSpecification : public CollectionGroup {
  std::optional<uint64_t> version;
};

struct CollectionGroupPlanSpecification : public CollectionGroup {
  struct ShardSheaf {
    LogId replicatedLog;
  };
  std::vector<ShardSheaf> shardSheaves;
};

struct CollectionGroupCurrentSpecification {
  struct Supervision {
    std::optional<uint64_t> version;
  };

  Supervision supervision;
};

/***
 * SECTION Collections
 */

struct Collection {
  CollectionGroupId groupId;

  struct MutableProperties {
    // TODO: This can be optimized into it's own struct.
    // Did a short_cut here to avoid concatenated changes
    arangodb::velocypack::Builder computedValues{VPackSlice::nullSlice()};

    // TODO: This can be optimized into it's own struct.
    // Did a short_cut here to avoid concatenated changes
    std::optional<arangodb::velocypack::Builder> schema{std::nullopt};

    bool cacheEnabled{false};

    bool operator==(MutableProperties const& other) const noexcept;
  };

  MutableProperties mutableProperties;

  struct ImmutableProperties : public CollectionInternalProperties {
    std::string name{StaticStrings::Empty};
    bool isSystem{false};
    std::underlying_type_t<TRI_col_type_e> type =
        TRI_col_type_e::TRI_COL_TYPE_DOCUMENT;
    KeyGeneratorProperties keyOptions{};
    bool isSmart{false};
    bool isDisjoint{false};
    std::string shardingStrategy{""};
    std::vector<std::string> shardKeys{};
    inspection::NonNullOptional<std::string> smartJoinAttribute{std::nullopt};
    inspection::NonNullOptional<std::string> smartGraphAttribute{std::nullopt};
    inspection::NonNullOptional<std::vector<DataSourceId>> shadowCollections{
        std::nullopt};
  };

  ImmutableProperties immutableProperties;

  CollectionIndexesProperties indexes;
};

struct CollectionTargetSpecification : public Collection {};

struct CollectionPlanSpecification : public Collection {
  std::vector<ShardID> shardList;

  // Note this is still here for compatibility, and temporary reasons.
  // We think we can get away with just above shardList and CollectionGroups
  // as soon as everything is in place.
  PlanShardToServerMapping deprecatedShardMap;
};

struct CollectionCurrentShardSpecification {
  bool error;
  std::string errorMessage;
  std::uint64_t errorNum;
  std::vector<velocypack::SharedSlice> indexes;
  std::vector<ServerID> servers;
  std::vector<ServerID> failoverCandidates;
};
struct CollectionCurrentSpecification {
  std::unordered_map<ShardID, CollectionCurrentShardSpecification> shards;
};

}  // namespace arangodb::replication2::agency

DECLARE_HASH_FOR_IDENTIFIER(arangodb::replication2::agency::CollectionGroupId)
DECLARE_EQUAL_FOR_IDENTIFIER(arangodb::replication2::agency::CollectionGroupId)
