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

#include <memory>
#include "Graph/BaseOptions.h"

namespace arangodb {

namespace aql {
class ExecutionPlan;
class Query;
}  // namespace aql

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack
namespace graph {
class EdgeCursor;

struct ShortestPathOptions : public BaseOptions {
 public:
  uint64_t minDepth;
  uint64_t maxDepth;
  std::string start;
  std::string end;
  bool bidirectional;
  bool multiThreaded;

  explicit ShortestPathOptions(aql::QueryContext& query);

  ShortestPathOptions(aql::QueryContext& query,
                      arangodb::velocypack::Slice const& info);

  // @brief DBServer-constructor used by TraverserEngines
  ShortestPathOptions(aql::QueryContext& query,
                      arangodb::velocypack::Slice info,
                      arangodb::velocypack::Slice collections);
  ~ShortestPathOptions() override;

  /// @brief This copy constructor is only working during planning phase.
  ///        After planning this node should not be copied anywhere.
  ///        When allowAlreadyBuiltCopy is true, the constructor also works
  ///        after the planning phase; however, the options have to be prepared
  ///        again (see ShortestPathNode / KShortestPathsNode
  ///        ::prepareOptions())
  ShortestPathOptions(ShortestPathOptions const& other,
                      bool allowAlreadyBuiltCopy = false);

  // Creates a complete Object containing all EngineInfo
  // in the given builder.
  void buildEngineInfo(arangodb::velocypack::Builder&) const override;

  std::unique_ptr<EdgeCursor> buildCursor(bool backward);

  /// @brief  Test if we have to use a weight attribute
  bool useWeight() const;

  /// @brief Build a velocypack for cloning in the plan.
  void toVelocyPack(arangodb::velocypack::Builder&) const override;

  // Creates a complete Object containing all index information
  // in the given builder.
  void toVelocyPackIndexes(arangodb::velocypack::Builder&) const override;

  /// @brief Estimate the total cost for this operation
  double estimateCost(size_t& nrItems) const override;

  // Creates a complete Object containing all EngineInfo
  // in the given builder.
  void addReverseLookupInfo(aql::ExecutionPlan* plan,
                            std::string const& collectionName,
                            std::string const& attributeName,
                            aql::AstNode* condition, bool onlyEdgeIndexes,
                            TRI_edge_direction_e direction);

  // Compute the weight of the given edge
  double weightEdge(arangodb::velocypack::Slice const) const;

  auto estimateDepth() const noexcept -> uint64_t override;

  auto setWeightAttribute(std::string attribute) -> void;
  auto getWeightAttribute() const& -> std::string;
  auto setDefaultWeight(double weight) -> void;
  auto getDefaultWeight() const -> double;

 private:
  /// @brief Lookup info to find all reverse edges.
  std::vector<LookupInfo> _reverseLookupInfos;
  std::string _weightAttribute;
  double _defaultWeight;

 public:
  /// This condition has to hold for all vertices on a
  /// found path
  /// Note that this struct, while called ShortestPathOptions is used
  /// for all path searches, i.e. ShortestPath, K-ShortestPaths, KPaths,
  /// AllShortestPaths, and that this expression is currently only used
  /// in the EnumeratePathsNode
  std::unique_ptr<aql::Expression> _allVerticesExpression;
  std::unique_ptr<aql::Expression> _allEdgesExpression;
};

}  // namespace graph
}  // namespace arangodb
