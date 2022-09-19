////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <Algorithm/Algorithm.h>
#include <Algorithm/Formatter.h>

#include <Inspection/VPack.h>

#include "Algorithm.h"
#include <string>
#include <variant>

namespace arangodb::pregel::algorithms::pagerank {

struct Settings {
  double epsilon;
  double dampingFactor;

  std::string resultField;
};
template <typename Inspector> auto inspect(Inspector &f, Settings &x) {
  return f.object(x).fields(f.field("epsilon", x.epsilon),
                            f.field("dampingFactor", x.dampingFactor),
                            f.field("resultField", x.resultField));
}

struct VertexProperties {
  double pageRank;
};
template <typename Inspector> auto inspect(Inspector &f, VertexProperties &x) {
  return f.object(x).fields(f.field("pageRank", x.pageRank));
}

struct Global {};

struct Message {
  double pageRank;
};

struct Aggregators {
  //  MaxAggregator<double> difference;
};

struct PageRankData {
  using Settings = Settings;
  using VertexProperties = VertexProperties;
  using EdgeProperties = algorithm_sdk::EmptyEdgeProperties;
  using Message = Message;

  using Global = Global;

  using Aggregators = Aggregators;
};

struct Topology : public algorithm_sdk::TopologyBase<PageRankData, Topology> {
  [[nodiscard]] auto readVertex(VPackSlice const &doc) -> VertexProperties {
    return VertexProperties{.pageRank = 0.0};
  }
  [[nodiscard]] auto readEdge(VPackSlice const &doc)
      -> algorithm_sdk::EmptyEdgeProperties {
    return algorithm_sdk::EmptyEdgeProperties{};
  }
};

struct Conductor
    : public algorithm_sdk::ConductorBase<PageRankData, Conductor> {
  Conductor(PageRankData::Settings settings) : ConductorBase(settings) {}
  auto setup() -> Global { return Global{}; }
  auto step(Global const &global) -> Global { return global; }
};

struct VertexComputation
    : public algorithm_sdk::VertexComputationBase<PageRankData,
                                                  VertexComputation> {};

} // namespace arangodb::pregel::algorithms::pagerank
