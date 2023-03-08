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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include "Pregel/Algorithm.h"

namespace arangodb {
namespace pregel {
namespace algos {

struct SSSPType {
  using Vertex = int64_t;
  using Edge = int64_t;
  using Message = int64_t;
};

/// Single Source Shortest Path. Uses integer attribute 'value', the source
/// should have
/// the value == 0, all others -1 or an undefined value
class SSSPAlgorithm : public Algorithm<int64_t, int64_t, int64_t> {
  std::string _sourceDocumentId, _resultField = "result";

 public:
  explicit SSSPAlgorithm(VPackSlice userParams) : Algorithm("sssp") {
    if (!userParams.isObject() || !userParams.hasKey("source")) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "You need to specify the source document id");
    }
    _sourceDocumentId = userParams.get("source").copyString();

    VPackSlice slice = userParams.get("resultField");
    if (slice.isString()) {
      _resultField = slice.copyString();
    } else {
      VPackSlice slice = userParams.get("_resultField");
      if (slice.isString()) {
        _resultField = slice.copyString();
      }
    }
  }

  GraphFormat<int64_t, int64_t>* inputFormat() const override;

  MessageFormat<int64_t>* messageFormat() const override {
    return new IntegerMessageFormat<int64_t>();
  }

  MessageCombiner<int64_t>* messageCombiner() const override {
    return new MinCombiner<int64_t>();
  }

  VertexComputation<int64_t, int64_t, int64_t>* createComputation(
      std::shared_ptr<WorkerConfig const>) const override;
  VertexCompensation<int64_t, int64_t, int64_t>* createCompensation(
      std::shared_ptr<WorkerConfig const>) const override;

  uint32_t messageBatchSize(std::shared_ptr<WorkerConfig const> config,
                            MessageStats const& stats) const override;
};
}  // namespace algos
}  // namespace pregel
}  // namespace arangodb
