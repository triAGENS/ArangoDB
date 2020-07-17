////////////////////////////////////////////////////////////////////////////////
///
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
///
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_ALGOS_VERTEX_ACCUMULATORS_ACCUMULATORS_H
#define ARANGODB_PREGEL_ALGOS_VERTEX_ACCUMULATORS_ACCUMULATORS_H 1

#include <Pregel/Algos/VertexAccumulators/AbstractAccumulator.h>

#include <inttypes.h>

namespace arangodb {
namespace pregel {
namespace algos {

class MinIntAccumulator : public Accumulator<int64_t> {
public:
  void update(data_type&& v) override {
    LOG_DEVEL << " UND UPDATE! " << v;
    if (v < _value) {
      _value = v;
    }
  }
};

}  // namespace algos
}  // namespace pregel
}  // namespace arangodb

#endif
