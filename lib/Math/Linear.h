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
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>

#include "Assertions/Assert.h"

namespace arangodb {

template<typename T>
T dotProduct(std::vector<T> const& lhs, std::vector<T> const& rhs) {
  TRI_ASSERT(lhs.size() == rhs.size()) << lhs.size() << " != " << rhs.size();
  T result{0};
  for (std::size_t i{0}; i < lhs.size(); ++i) {
    result += lhs[i] * rhs[i];
  }
  return result;
}

}  // namespace arangodb
