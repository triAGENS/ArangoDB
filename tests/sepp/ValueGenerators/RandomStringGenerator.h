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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include "ValueGenerator.h"

#include "Basics/xoroshiro128plus.h"

namespace arangodb::sepp::generators {

struct RandomStringGenerator : ValueGenerator {
  RandomStringGenerator(std::uint32_t size);

  /// @brief Generates a value and writes it to the given Builder.
  void apply(velocypack::Builder&) override;

 private:
  std::uint32_t _size;
  basics::xoroshiro128plus _prng;
};

}  // namespace arangodb::sepp::generators
