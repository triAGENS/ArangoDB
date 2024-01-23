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
#pragma once

#include <optional>
#include <string>
#include <velocypack/SharedSlice.h>

#include <Basics/StaticStrings.h>

namespace arangodb::replication2::agency {

struct ImplementationSpec {
  std::string type;
  std::optional<velocypack::SharedSlice> parameters;

  friend auto operator==(ImplementationSpec const& s,
                         ImplementationSpec const& s2) noexcept -> bool;
};

auto operator==(ImplementationSpec const& s,
                ImplementationSpec const& s2) noexcept -> bool;

template<class Inspector>
auto inspect(Inspector& f, ImplementationSpec& x) {
  return f.object(x).fields(
      f.field(StaticStrings::IndexType, x.type),
      f.field(StaticStrings::DataSourceParameters, x.parameters));
}

}  // namespace arangodb::replication2::agency
