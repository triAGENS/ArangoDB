////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Heiko Kernbach
/// @author Lars Maier
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "VertexAccumulators.h"

#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Pregel/Algorithm.h"
#include "Pregel/GraphStore.h"
#include "Pregel/IncomingCache.h"
#include "Pregel/VertexComputation.h"

#include "Pregel/Worker.h"

#include "Logger/Logger.h"

#include "Pregel/Algos/VertexAccumulators/AccumulatorOptionsDeserializer.h"

#include <set>

using namespace arangodb;
using namespace arangodb::pregel;
using namespace arangodb::pregel::algos;

std::ostream& arangodb::pregel::algos::operator<<(std::ostream& os, VertexData const& vertexData) {
  os << vertexData.toString();
  return os;
}

VertexAccumulators::VertexAccumulators(application_features::ApplicationServer& server,
                                       VPackSlice userParams)
    : Algorithm(server, "VertexAccumulators") {
  LOG_DEVEL << "VertexAccumulators Pregel Algorithm created";

  parseUserParams(userParams);

  LOG_DEVEL << "done.";
}

bool VertexAccumulators::supportsAsyncMode() const { return false; }
bool VertexAccumulators::supportsCompensation() const { return false; }

auto VertexAccumulators::createComputation(WorkerConfig const* config) const
    -> vertex_computation* {
  return new VertexComputation();
}

auto VertexAccumulators::inputFormat() const -> graph_format* {
  // TODO: The resultField needs to be configurable from the user's side
  return new GraphFormat(_server, _options.resultField);
}

void VertexAccumulators::parseUserParams(VPackSlice userParams) {
  LOG_DEVEL << "parsing user params: " << userParams;
  auto result = parseVertexAccumulatorOptions(userParams);
  if(result) {
    _options = std::move(result).get();
  } else {
    // What do we do on error? std::terminate()
    LOG_DEVEL << result.error().as_string();
  }
}


template class arangodb::pregel::Worker<VertexData, EdgeData, MessageData>;
