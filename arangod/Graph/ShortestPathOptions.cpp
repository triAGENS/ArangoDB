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

#include "ShortestPathOptions.h"

#include "Aql/Query.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterEdgeCursor.h"
#include "Cluster/ClusterMethods.h"
#include "Graph/ClusterTraverserCache.h"
#include "Graph/SingleServerEdgeCursor.h"
#include "Indexes/Index.h"
#include "Transaction/Helpers.h"

#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::graph;
using namespace arangodb::traverser;
using VPackHelper = arangodb::basics::VelocyPackHelper;

ShortestPathOptions::ShortestPathOptions(aql::QueryContext& query)
    : BaseOptions(query),
      minDepth(1),
      maxDepth(1),
      bidirectional(true),
      multiThreaded(true) {
  setWeightAttribute("");
  setDefaultWeight(1);
}

ShortestPathOptions::ShortestPathOptions(aql::QueryContext& query,
                                         VPackSlice const& info)
    : ShortestPathOptions(query) {
  TRI_ASSERT(info.isObject());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  VPackSlice type = info.get("type");
  TRI_ASSERT(type.isString());
  TRI_ASSERT(type.isEqualString("shortestPath"));
#endif
  parseShardIndependentFlags(info);
  minDepth = VPackHelper::getNumericValue<uint64_t>(info, "minDepth", 1);
  maxDepth = VPackHelper::getNumericValue<uint64_t>(info, "maxDepth", 1);

  setWeightAttribute(
      VelocyPackHelper::getStringValue(info, "weightAttribute", ""));
  setDefaultWeight(
      VelocyPackHelper::getNumericValue<double>(info, "defaultWeight", 1));
  setProduceVertices(
      VPackHelper::getBooleanValue(info, "produceVertices", true));
}

ShortestPathOptions::ShortestPathOptions(aql::QueryContext& query,
                                         VPackSlice info,
                                         VPackSlice collections)
    : BaseOptions(query, info, collections),
      bidirectional(true),
      multiThreaded(true) {
  TRI_ASSERT(info.isObject());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  VPackSlice type = info.get("type");
  TRI_ASSERT(type.isString());
  TRI_ASSERT(type.isEqualString("shortestPath"));
#endif
  minDepth = VPackHelper::getNumericValue<uint64_t>(info, "minDepth", 1);
  maxDepth = VPackHelper::getNumericValue<uint64_t>(info, "maxDepth", 1);

  setWeightAttribute(
      VelocyPackHelper::getStringValue(info, "weightAttribute", ""));
  setDefaultWeight(
      VelocyPackHelper::getNumericValue<double>(info, "defaultWeight", 1));
  setProduceVertices(
      VPackHelper::getBooleanValue(info, "produceVertices", true));

  VPackSlice read = info.get("reverseLookupInfos");
  if (!read.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a reverseLookupInfos");
  }

  size_t length = read.length();
  TRI_ASSERT(read.length() == collections.length());
  _reverseLookupInfos.reserve(length);
  for (size_t j = 0; j < length; ++j) {
    _reverseLookupInfos.emplace_back(query, read.at(j), collections.at(j));
  }

  read = info.get("baseVertexExpression");
  if (!read.isNone()) {
    if (!read.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "The options require baseVertexExpression to be an object");
    }
    _allVerticesExpression =
        std::make_unique<aql::Expression>(query.ast(), read);
  }

  read = info.get("baseEdgeExpression");
  if (!read.isNone()) {
    if (!read.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "The options require baseEdgeExpression to be an object");
    }
    _allEdgesExpression = std::make_unique<aql::Expression>(query.ast(), read);
  }
}

ShortestPathOptions::~ShortestPathOptions() = default;

void ShortestPathOptions::buildEngineInfo(VPackBuilder& result) const {
  result.openObject();
  injectEngineInfo(result);
  result.add("type", VPackValue("shortestPath"));
  result.add("defaultWeight", VPackValue(getDefaultWeight()));
  result.add("weightAttribute", VPackValue(getWeightAttribute()));
  result.add(VPackValue("reverseLookupInfos"));
  result.openArray();
  for (auto const& it : _reverseLookupInfos) {
    it.buildEngineInfo(result);
  }
  result.close();

  result.close();
}

bool ShortestPathOptions::useWeight() const {
  return !_weightAttribute.empty();
}

void ShortestPathOptions::toVelocyPack(VPackBuilder& builder) const {
  VPackObjectBuilder guard(&builder);
  toVelocyPackBase(builder);
  builder.add("minDepth", VPackValue(minDepth));
  builder.add("maxDepth", VPackValue(maxDepth));
  builder.add("weightAttribute", VPackValue(getWeightAttribute()));
  builder.add("defaultWeight", VPackValue(getDefaultWeight()));
  builder.add("produceVertices", VPackValue(produceVertices()));
  builder.add("type", VPackValue("shortestPath"));

  if (_allVerticesExpression != nullptr) {
    builder.add(VPackValue("baseVertexExpression"));
    builder.openObject();
    builder.add(VPackValue("expression"));
    _allVerticesExpression->toVelocyPack(builder, true);
    builder.close();
  }

  if (_allEdgesExpression != nullptr) {
    builder.add(VPackValue("baseEdgeExpression"));
    builder.openObject();
    builder.add(VPackValue("expression"));
    _allEdgesExpression->toVelocyPack(builder, true);
    builder.close();
  }
}

void ShortestPathOptions::toVelocyPackIndexes(VPackBuilder& builder) const {
  VPackObjectBuilder guard(&builder);

  // base indexes
  builder.add("base", VPackValue(VPackValueType::Array));
  for (auto const& it : _baseLookupInfos) {
    for (auto const& it2 : it.idxHandles) {
      it2->toVelocyPack(builder, Index::makeFlags(Index::Serialize::Basics,
                                                  Index::Serialize::Estimates));
    }
  }
  builder.close();
}

double ShortestPathOptions::estimateCost(size_t& nrItems) const {
  size_t baseCreateItems = 0;
  double baseCost = costForLookupInfoList(_baseLookupInfos, baseCreateItems);
  // We use the "seven-degrees-of-seperation" rule.
  // This theory asumes that the shortest path is at most 7 steps of length

  nrItems = static_cast<size_t>(std::pow(baseCreateItems, 7));
  return std::pow(baseCost, 7);
}

void ShortestPathOptions::addReverseLookupInfo(
    aql::ExecutionPlan* plan, std::string const& collectionName,
    std::string const& attributeName, aql::AstNode* condition,
    bool onlyEdgeIndexes, TRI_edge_direction_e direction) {
  injectLookupInfoInList(_reverseLookupInfos, plan, collectionName,
                         attributeName, condition, onlyEdgeIndexes, direction);
}

double ShortestPathOptions::weightEdge(VPackSlice edge) const {
  TRI_ASSERT(useWeight());
  const auto weight =
      arangodb::basics::VelocyPackHelper::getNumericValue<double>(
          edge, _weightAttribute.c_str(), _defaultWeight);
  if (weight < 0.) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT);
  }

  return weight;
}

std::unique_ptr<EdgeCursor> ShortestPathOptions::buildCursor(bool backward) {
  ensureCache();

  if (_isCoordinator) {
    return std::make_unique<ClusterShortestPathEdgeCursor>(this, backward);
  }

  return std::make_unique<SingleServerEdgeCursor>(
      this, _tmpVar, nullptr,
      backward ? _reverseLookupInfos : _baseLookupInfos);
}

auto ShortestPathOptions::estimateDepth() const noexcept -> uint64_t {
  // We certainly have no clue how the depth actually is.
  // So we return a "random" number here.
  // By the six degrees of seperation rule, which defines most vertices in a
  // naturally created graph are 6 steps away from each other, 7 seems to be a
  // quite good worst-case estimate.
  return 7;
}

auto ShortestPathOptions::setDefaultWeight(double weight) -> void {
  if (weight < 0.) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_GRAPH_NEGATIVE_EDGE_WEIGHT,
                                   "negative default weight not allowed");
  }
  _defaultWeight = weight;
}

auto ShortestPathOptions::setWeightAttribute(std::string attribute) -> void {
  _weightAttribute = std::move(attribute);
}

auto ShortestPathOptions::getDefaultWeight() const -> double {
  TRI_ASSERT(_defaultWeight > .0);
  return _defaultWeight;
}

auto ShortestPathOptions::getWeightAttribute() const& -> std::string {
  return _weightAttribute;
}

ShortestPathOptions::ShortestPathOptions(ShortestPathOptions const& other,
                                         bool const allowAlreadyBuiltCopy)
    : BaseOptions(other, allowAlreadyBuiltCopy),
      minDepth(other.minDepth),
      maxDepth(other.maxDepth),
      start{other.start},
      end{other.end},
      bidirectional{other.bidirectional},
      multiThreaded{other.multiThreaded},
      _reverseLookupInfos{other._reverseLookupInfos},
      _weightAttribute{other._weightAttribute},
      _defaultWeight{other._defaultWeight} {
  TRI_ASSERT(other._defaultWeight >= 0.);
}
