////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2022-2022 ArangoDB GmbH, Cologne, Germany
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

#include "PlanCollection.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ClusterFeature.h"
#include "Inspection/VPack.h"
#include "Logger/LogMacros.h"
#include "RestServer/DatabaseFeature.h"
#include "Utilities/NameValidator.h"
#include "VocBase/vocbase.h"

#include <velocypack/Collection.h>
#include <velocypack/Slice.h>

using namespace arangodb;

namespace {
PlanCollection initWithDefaults(
    PlanCollection::DatabaseConfiguration const& config,
    std::string const& name) {
  PlanCollection res;
  // Inject certain default values.
  res.name = name;

  // This will make sure the default configuration for Sharding attributes are
  // applied
  res.numberOfShards = config.defaultNumberOfShards;
  res.replicationFactor = config.defaultReplicationFactor;
  res.writeConcern = config.defaultWriteConcern;
  res.distributeShardsLike = config.defaultDistributeShardsLike;

  return res;
}

ResultT<PlanCollection> parseAndValidate(
    PlanCollection::DatabaseConfiguration const& config, VPackSlice input,
    std::string const& defaultName,
    std::function<void(PlanCollection&)> applyOnSuccess) {
  try {
    PlanCollection res = initWithDefaults(config, defaultName);
    auto status = velocypack::deserializeWithStatus(input, res);
    if (status.ok()) {
      // TODO: We can actually call validateDatabaseConfiguration
      // here, to make sure everything is correct from the very beginning
      applyOnSuccess(res);
      return res;
    }
    if (status.path() == "name") {
      // Special handling to be backwards compatible error reporting
      // on "name"
      return Result{TRI_ERROR_ARANGO_ILLEGAL_NAME};
    }
    if (status.path() == StaticStrings::SmartJoinAttribute) {
      return Result{TRI_ERROR_INVALID_SMART_JOIN_ATTRIBUTE, status.error()};
    }
    return Result{
        TRI_ERROR_BAD_PARAMETER,
        status.error() +
            (status.path().empty() ? "" : " on path " + status.path())};
  } catch (basics::Exception const& e) {
    return Result{e.code(), e.message()};
  } catch (std::exception const& e) {
    return Result{TRI_ERROR_INTERNAL, e.what()};
  }
}

}  // namespace

PlanCollection::DatabaseConfiguration::DatabaseConfiguration(
    TRI_vocbase_t const& vocbase) {
  auto& server = vocbase.server();
  auto& cl = server.getFeature<ClusterFeature>();
  auto& db = server.getFeature<DatabaseFeature>();
  maxNumberOfShards = cl.maxNumberOfShards();
  allowExtendedNames = db.extendedNamesForCollections();
  shouldValidateClusterSettings = true;
  minReplicationFactor = cl.minReplicationFactor();
  maxReplicationFactor = cl.maxReplicationFactor();
  enforceReplicationFactor = false;
  defaultNumberOfShards = 1;
  defaultReplicationFactor =
      std::max(vocbase.replicationFactor(), cl.systemReplicationFactor());
  defaultWriteConcern = vocbase.writeConcern();

  isOneShardDB = cl.forceOneShard() || vocbase.isOneShard();
  if (isOneShardDB) {
    defaultDistributeShardsLike = vocbase.shardingPrototypeName();
  } else {
    defaultDistributeShardsLike = "";
  }
}

PlanCollection::PlanCollection() {}

ResultT<PlanCollection> PlanCollection::fromCreateAPIBody(
    VPackSlice input, DatabaseConfiguration config) {
  if (!input.isObject()) {
    // Special handling to be backwards compatible error reporting
    // on "name"
    return Result{TRI_ERROR_ARANGO_ILLEGAL_NAME};
  }
  return ::parseAndValidate(config, input, StaticStrings::Empty,
                            [](PlanCollection&) {});
}

ResultT<PlanCollection> PlanCollection::fromCreateAPIV8(
    VPackSlice properties, std::string const& name, TRI_col_type_e type,
    DatabaseConfiguration config) {
  if (name.empty()) {
    // Special handling to be backwards compatible error reporting
    // on "name"
    return Result{TRI_ERROR_ARANGO_ILLEGAL_NAME};
  }
  return ::parseAndValidate(config, properties, name,
                            [&name, &type](PlanCollection& col) {
                              // If we have given a type, it always wins.
                              // As we hand in an enum the type has to be valid.
                              // TODO: Should we silently do this?
                              // or should we throw an illegal use error?
                              col.type = type;
                              col.name = name;
                            });
}

arangodb::velocypack::Builder PlanCollection::toCreateCollectionProperties(
    std::vector<PlanCollection> const& collections) {
  arangodb::velocypack::Builder builder;
  VPackArrayBuilder guard{&builder};
  for (auto const& c : collections) {
    // TODO: This is copying multiple times.
    // Nevermind this is only temporary code.
    builder.add(c.toCollectionsCreate().slice());
  }
  return builder;
}

auto PlanCollection::Invariants::isNonEmpty(std::string const& value)
    -> inspection::Status {
  if (value.empty()) {
    return {"Value cannot be empty."};
  }
  return inspection::Status::Success{};
}

auto PlanCollection::Invariants::isNonEmptyIfPresent(
    std::optional<std::string> const& value) -> inspection::Status {
  if (value.has_value() && value.value().empty()) {
    return {"Value cannot be empty."};
  }
  return inspection::Status::Success{};
}

auto PlanCollection::Invariants::isGreaterZero(uint64_t const& value)
    -> inspection::Status {
  if (value > 0) {
    return inspection::Status::Success{};
  }
  return {"Value has to be > 0"};
}

auto PlanCollection::Invariants::isValidShardingStrategy(
    std::string const& strat) -> inspection::Status {
  // Note we may be better off with a lookup list here
  // Hash is first on purpose (default)
  if (strat == "" || strat == "hash" || strat == "enterprise-hash-smart-edge" ||
      strat == "community-compat" || strat == "enterprise-compat" ||
      strat == "enterprise-smart-edge-compat") {
    return inspection::Status::Success{};
  }
  return {
      "Please use 'hash' or remove, advanced users please "
      "pick a strategy from the documentation, " +
      strat + " is not allowed."};
}

auto PlanCollection::Invariants::isValidCollectionType(
    std::underlying_type_t<TRI_col_type_e> const& type) -> inspection::Status {
  if (type == TRI_col_type_e::TRI_COL_TYPE_DOCUMENT ||
      type == TRI_col_type_e::TRI_COL_TYPE_EDGE) {
    return inspection::Status::Success{};
  }
  return {"Only 2 (document) and 3 (edge) are allowed."};
}

auto PlanCollection::Invariants::areShardKeysValid(
    std::vector<std::string> const& keys) -> inspection::Status {
  if (keys.empty() || keys.size() > 8) {
    return {"invalid number of shard keys for collection"};
  }
  for (auto const& sk : keys) {
    auto key = std::string_view{sk};
    // remove : char at the beginning or end (for enterprise)
    std::string_view stripped;
    if (!key.empty()) {
      if (key.front() == ':') {
        stripped = key.substr(1);
      } else if (key.back() == ':') {
        stripped = key.substr(0, key.size() - 1);
      } else {
        stripped = key;
      }
    }
    // system attributes are not allowed (except _key, _from and _to)
    if (stripped == StaticStrings::IdString ||
        stripped == StaticStrings::RevString) {
      return {"_id or _rev cannot be used as shard keys"};
    }
  }
  return inspection::Status::Success{};
}

auto PlanCollection::Transformers::ReplicationSatellite::toSerialized(
    MemoryType v, SerializedType& result) -> arangodb::inspection::Status {
  result.add(VPackValue(v));
  return {};
}

auto PlanCollection::Transformers::ReplicationSatellite::fromSerialized(
    SerializedType const& b, MemoryType& result)
    -> arangodb::inspection::Status {
  auto v = b.slice();
  if (v.isString() && v.isEqualString("satellite")) {
    result = 0;
    return {};
  } else if (v.isNumber()) {
    result = v.getNumber<MemoryType>();
    if (result != 0) {
      return {};
    }
  }
  return {"Only an integer number or 'satellite' is allowed"};
}

arangodb::Result PlanCollection::validateDatabaseConfiguration(
    DatabaseConfiguration config) const {
  //  Check name is allowed
  if (!CollectionNameValidator::isAllowedName(
          isSystem, config.allowExtendedNames, name)) {
    return {TRI_ERROR_ARANGO_ILLEGAL_NAME};
  }
  if (config.shouldValidateClusterSettings) {
    if (config.maxNumberOfShards > 0 &&
        numberOfShards > config.maxNumberOfShards) {
      return {TRI_ERROR_CLUSTER_TOO_MANY_SHARDS,
              std::string("too many shards. maximum number of shards is ") +
                  std::to_string(config.maxNumberOfShards)};
    }
  }

  // Check Replication factor
  if (config.enforceReplicationFactor) {
    if (config.maxReplicationFactor > 0 &&
        replicationFactor > config.maxReplicationFactor) {
      return {TRI_ERROR_BAD_PARAMETER,
              std::string("replicationFactor must not be higher than "
                          "maximum allowed replicationFactor (") +
                  std::to_string(config.maxReplicationFactor) + ")"};
    }

    if (replicationFactor != 0 && replicationFactor < config.minReplicationFactor) {
      return {TRI_ERROR_BAD_PARAMETER,
              std::string("replicationFactor must not be lower than "
                          "minimum allowed replicationFactor (") +
                  std::to_string(config.minReplicationFactor) + ")"};
    }

    if (replicationFactor > 0 && replicationFactor < writeConcern) {
      return {TRI_ERROR_BAD_PARAMETER,
              "writeConcern must not be higher than replicationFactor"};
    }
  }

  if (replicationFactor == 0) {
    // We are a satellite, we cannot be smart at the same time
    if (isSmart) {
      return {TRI_ERROR_BAD_PARAMETER,
              "'isSmart' and replicationFactor 'satellite' cannot be combined"};
    }
    if (isSmartChild) {
      return {TRI_ERROR_BAD_PARAMETER,
              "'isSmartChild' and replicationFactor 'satellite' cannot be combined"};
    }
    if (shardKeys.size() != 1 || shardKeys[0] != StaticStrings::KeyString) {
      return {TRI_ERROR_BAD_PARAMETER, "'satellite' cannot use shardKeys"};
    }
  }
  if (config.isOneShardDB) {
    if (numberOfShards != 1) {
      return {TRI_ERROR_BAD_PARAMETER,
              "Collection in a 'oneShardDatabase' must have 1 shard"};
    }

    if (distributeShardsLike != config.defaultDistributeShardsLike) {
      return {TRI_ERROR_BAD_PARAMETER,
              "Collection in a 'oneShardDatabase' cannot define "
              "'distributeShardsLike'"};
    }

    if (replicationFactor == 0) {
      return {TRI_ERROR_BAD_PARAMETER,
              "Collection in a 'oneShardDatabase' cannot be a "
              "'satellite'"};
    }
  }

  return {};
}

arangodb::velocypack::Builder PlanCollection::toCollectionsCreate() const {
  arangodb::velocypack::Builder builder;
  arangodb::velocypack::serialize(builder, *this);
  // TODO: This is a hack to erase attributes that are not expected by follow up
  // APIS, it should be obsolete after refactoring is completed
  std::vector<std::string> attributesToErase{};
  if (builder.slice().hasKey(StaticStrings::SmartJoinAttribute) &&
      builder.slice()
          .get(StaticStrings::SmartJoinAttribute)
          .isEqualString("")) {
    // TODO: This is a hack to erase the SmartJoin attribute if it was not set
    // We need this to satisfy the API checks in LogicalCollection
    // `initializeSmartAttributes`
    attributesToErase.emplace_back(StaticStrings::SmartJoinAttribute);
  }
  if (builder.slice().hasKey(StaticStrings::ShardingStrategy) &&
      builder.slice().get(StaticStrings::ShardingStrategy).isEqualString("")) {
    // TODO: This is a hack to erase the ShardingStrategy attribute if it was
    // not set
    attributesToErase.emplace_back(StaticStrings::ShardingStrategy);
  }

  if (builder.slice().hasKey(StaticStrings::GraphSmartGraphAttribute) &&
      builder.slice()
          .get(StaticStrings::GraphSmartGraphAttribute)
          .isEqualString("")) {
    // TODO: This is a hack to erase the SmartGraphAttribute attribute if it was
    // not set
    attributesToErase.emplace_back(StaticStrings::GraphSmartGraphAttribute);
  }
  if (!attributesToErase.empty()) {
    return VPackCollection::remove(builder.slice(), attributesToErase);
  }
  return builder;
}
