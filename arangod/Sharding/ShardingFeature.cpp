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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ShardingFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Sharding/ShardingInfo.h"
#include "Sharding/ShardingStrategyDefault.h"
#include "VocBase/LogicalCollection.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/Sharding/ShardingStrategyEE.h"
#endif

#include <array>

using namespace arangodb::application_features;
using namespace arangodb::basics;

namespace arangodb {

ShardingFeature::ShardingFeature(Server& server)
    : ArangodFeature{server, *this} {
  setOptional(false);
  startsAfter<GreetingsFeaturePhase>();
}

void ShardingFeature::prepare() {
  registerFactory(ShardingStrategyNone::NAME, [](ShardingInfo*) {
    return std::make_unique<ShardingStrategyNone>();
  });
  registerFactory(
      ShardingStrategyCommunityCompat::NAME, [](ShardingInfo* sharding) {
        return std::make_unique<ShardingStrategyCommunityCompat>(sharding);
      });
  // note: enterprise-compat is always there so users can downgrade from
  // Enterprise Edition to Community Edition
  registerFactory(
      ShardingStrategyEnterpriseCompat::NAME, [](ShardingInfo* sharding) {
        return std::make_unique<ShardingStrategyEnterpriseCompat>(sharding);
      });
  registerFactory(ShardingStrategyHash::NAME, [](ShardingInfo* sharding) {
    return std::make_unique<ShardingStrategyHash>(sharding);
  });
#ifdef USE_ENTERPRISE
  // the following sharding strategies are only available in the
  // Enterprise Edition
  registerFactory(
      ShardingStrategyEnterpriseSmartEdgeCompat::NAME,
      [](ShardingInfo* sharding) {
        return std::make_unique<ShardingStrategyEnterpriseSmartEdgeCompat>(
            sharding);
      });
  registerFactory(
      ShardingStrategyEnterpriseHashSmartEdge::NAME,
      [](ShardingInfo* sharding) {
        return std::make_unique<ShardingStrategyEnterpriseHashSmartEdge>(
            sharding);
      });
  registerFactory(
      ShardingStrategyEnterpriseHexSmartVertex::NAME,
      [](ShardingInfo* sharding) {
        return std::make_unique<ShardingStrategyEnterpriseHexSmartVertex>(
            sharding);
      });
#else
  // in the Community Edition register some stand-ins for the sharding
  // strategies only available in the Enterprise Edition
  // note: these standins will actually not do any sharding, but always
  // throw an exception telling the user that the selected sharding
  // strategy is only available in the Enterprise Edition
  for (auto const& name : std::array<std::string, 3>{
           "enterprise-smart-edge-compat", "enterprise-hash-smart-edge",
           "enterprise-hex-smart-vertex"}) {
    registerFactory(name, [&name](ShardingInfo* sharding) {
      return std::make_unique<ShardingStrategyOnlyInEnterprise>(name);
    });
  }
#endif
}

void ShardingFeature::start() {
  std::vector<std::string> strategies;
  for (auto const& it : _factories) {
    strategies.emplace_back(it.first);
  }
  LOG_TOPIC("2702f", TRACE, Logger::CLUSTER)
      << "supported sharding strategies: " << strategies;
}

void ShardingFeature::registerFactory(
    std::string const& name, ShardingStrategy::FactoryFunction const& creator) {
  LOG_TOPIC("69525", TRACE, Logger::CLUSTER)
      << "registering sharding strategy '" << name << "'";

  if (!_factories.emplace(name, creator).second) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, absl::StrCat("sharding factory function '", name,
                                         "' already registered"));
  }
}

std::unique_ptr<ShardingStrategy> ShardingFeature::fromVelocyPack(
    VPackSlice slice, ShardingInfo* sharding) {
  if (!slice.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "invalid collection meta data");
  }

  std::string name;
  // determine the correct method for sharding
  VPackSlice s = slice.get("shardingStrategy");
  if (s.isString()) {
    name = s.copyString();
  } else {
    name = getDefaultShardingStrategy(sharding);
  }

  return create(name, sharding);
}

std::string ShardingFeature::getDefaultShardingStrategy(
    ShardingInfo const* sharding) const {
  // TODO change these to use better algorithms when we no longer
  //      need to support collections created before 3.4

  // before 3.4, there were only hard-coded sharding strategies

  // no sharding strategy found in collection meta data
#ifdef USE_ENTERPRISE
  if (sharding->collection()->isSmart() &&
      sharding->collection()->type() == TRI_COL_TYPE_EDGE) {
    // smart edge collection
    return ShardingStrategyEnterpriseSmartEdgeCompat::NAME;
  }

  return ShardingStrategyEnterpriseCompat::NAME;
#else
  return ShardingStrategyCommunityCompat::NAME;
#endif
}

std::unique_ptr<ShardingStrategy> ShardingFeature::create(
    std::string const& name, ShardingInfo* sharding) {
  auto it = _factories.find(name);

  if (it == _factories.end()) {
    std::string_view hint;
#ifndef USE_ENTERPRISE
    if (name.find("enterprise") != std::string::npos) {
      hint =
          " - sharding strategy is only available in the Enterprise "
          "Edition of ArangoDB";
    }
#endif

    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        absl::StrCat("unknown sharding type '", name, "'", hint));
  }

  // now create a sharding strategy instance
  return (*it).second(sharding);
}

std::string ShardingFeature::getDefaultShardingStrategyForNewCollection(
    VPackSlice const& properties) const {
  // from 3.4 onwards, the default sharding strategy for new collections is
  // "hash"
#ifdef USE_ENTERPRISE
  bool isSmart = VelocyPackHelper::getBooleanValue(
      properties, StaticStrings::IsSmart, false);
  bool isEdge =
      TRI_COL_TYPE_EDGE == VelocyPackHelper::getNumericValue<uint32_t>(
                               properties, "type", TRI_COL_TYPE_DOCUMENT);
  if (isSmart) {
    if (isEdge) {
      // Smart Edge Collection
      return ShardingStrategyEnterpriseHashSmartEdge::NAME;
    } else {
      // Smart Vertex Collection
      // We need to differentiate between SmartGraphs and EnterpriseGraphs here.
      VPackSlice sga = properties.get(StaticStrings::GraphSmartGraphAttribute);
      if (sga.isNone()) {
        // Means we're in the EnterpriseGraph Case.
        // In case we do have a SmartVertex collection without a
        // SmartGraphAttribute given, we use a different sharding strategy.
        // In case it is given, we fall back to the default ShardingStrategyHash
        // strategy.
        return ShardingStrategyEnterpriseHexSmartVertex::NAME;
      } else {
        // Means we're in the SmartGraph case.
        return ShardingStrategyHash::NAME;
      }
    }
  }
#endif

  // Info: Satellite collections will use this ShardingStrategy as well.
  return ShardingStrategyHash::NAME;
}

}  // namespace arangodb
