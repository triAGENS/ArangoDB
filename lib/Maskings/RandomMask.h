////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Maskings/RandomStringMask.h"

namespace arangodb {
namespace maskings {
class RandomMask : public RandomStringMask {
 public:
  static ParseResult<AttributeMasking> create(Path, Maskings*, VPackSlice const& def);

 public:
  VPackValue mask(bool, std::string& buffer) const override;
  VPackValue mask(int64_t, std::string& buffer) const override;
  VPackValue mask(double, std::string& buffer) const override;

 private:
  explicit RandomMask(Maskings* maskings) : RandomStringMask(maskings) {}
};
}  // namespace maskings
}  // namespace arangodb
