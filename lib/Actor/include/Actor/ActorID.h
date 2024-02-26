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
/// @author Julia Volmer
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <string>
#include "Inspection/Format.h"

namespace arangodb::actor {

struct ActorID {
  size_t id;

  auto operator<=>(ActorID const& other) const = default;
};

inline std::ostream& operator<<(std::ostream& o, ActorID const& id) {
  o << "ActorID(" << id.id << ")";
  return o;
}

template<typename Inspector>
auto inspect(Inspector& f, ActorID& x) {
  if constexpr (Inspector::isLoading) {
    auto v = size_t{0};
    auto res = f.apply(v);
    if (res.ok()) {
      x = ActorID{.id = v};
    }
    return res;
  } else {
    return f.apply(x.id);
  }
}
}  // namespace arangodb::actor

template<>
struct fmt::formatter<arangodb::actor::ActorID>
    : arangodb::inspection::inspection_formatter {};

namespace std {
template<>
struct hash<arangodb::actor::ActorID> {
  size_t operator()(arangodb::actor::ActorID const& x) const noexcept {
    return std::hash<size_t>()(x.id);
  };
};
}  // namespace std
