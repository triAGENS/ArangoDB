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

#include <chrono>
#include <cstdint>
#include <string>

#include <Inspection/VPack.h>
#include <Inspection/Transformers.h>

#include <Pregel/Common.h>

#include "StaticStrings.h"

using TimeStamp = std::chrono::system_clock::time_point;

namespace arangodb::pregel {
struct WorkerStatus {
  TimeStamp timeStamp;
  std::size_t verticesLoaded;
  std::size_t edgesLoaded;

  WorkerStatus()
      : timeStamp{std::chrono::system_clock::now()},
        verticesLoaded{0},
        edgesLoaded{0} {}
};

template<typename Inspector>
auto inspect(Inspector& f, WorkerStatus& x) {
  return f.object(x).fields(
      f.field(static_strings::timeStamp, x.timeStamp)
          .transformWith(inspection::TimeStampTransformer{}),
      f.field(static_strings::verticesLoaded, x.verticesLoaded),
      f.field(static_strings::edgesLoaded, x.edgesLoaded));
}

}  // namespace arangodb::pregel
