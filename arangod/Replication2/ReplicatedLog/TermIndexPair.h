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

#include "Replication2/ReplicatedLog/LogIndex.h"
#include "Replication2/ReplicatedLog/LogTerm.h"

namespace arangodb::velocypack {
class Builder;
}  // namespace arangodb::velocypack

namespace arangodb::replication2 {

struct TermIndexPair {
  LogTerm term{};
  LogIndex index{};

  TermIndexPair(LogTerm term, LogIndex index) noexcept;
  TermIndexPair() = default;

  void toVelocyPack(velocypack::Builder& builder) const;
  [[nodiscard]] static auto fromVelocyPack(velocypack::Slice) -> TermIndexPair;

  friend auto operator<=>(TermIndexPair const&, TermIndexPair const&) = default;
  friend auto operator<<(std::ostream&, TermIndexPair) -> std::ostream&;
};

auto operator<<(std::ostream&, TermIndexPair) -> std::ostream&;
[[nodiscard]] auto to_string(TermIndexPair pair) -> std::string;

template<class Inspector>
auto inspect(Inspector& f, TermIndexPair& x) {
  return f.object(x).fields(f.field("term", x.term), f.field("index", x.index));
}

}  // namespace arangodb::replication2
