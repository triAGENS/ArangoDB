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

#include <memory>
#include <vector>
#include <string>

namespace arangodb::pregel3 {

using VertexId = std::string;

struct Vertex {
  std::vector<size_t> neighbours;
};

struct Graph {
  std::vector<Vertex> vertices;
};

struct GraphSpecification {
  // ...
};

using QueryId = std::size_t;
struct Query {
  Query(QueryId id, GraphSpecification const& graphSpec)
      : id{id}, graphSpec{graphSpec} {};

  void loadGraph();

 private:
  QueryId id;
  GraphSpecification graphSpec;
  std::shared_ptr<Graph> graph{nullptr};
};

}  // namespace arangodb::pregel3
