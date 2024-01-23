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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "ImplementationSpec.h"

namespace arangodb::replication2::agency {

auto operator==(ImplementationSpec const& s,
                ImplementationSpec const& s2) noexcept -> bool {
  if (s.type != s2.type ||
      s.parameters.has_value() != s2.parameters.has_value()) {
    return false;
  }
  return !s.parameters.has_value();
  // To compare two velocypacks ICU is required. For unittests we don't want
  // to have that dependency and unless we build a non-icu variant,
  // comparing here is not possible.
  /*         basics::VelocyPackHelper::equal(s.parameters->slice(),
                                           s2.parameters->slice(), true);*/
}

}  // namespace arangodb::replication2::agency
