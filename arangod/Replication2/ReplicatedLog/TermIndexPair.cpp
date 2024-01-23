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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "LogCommon.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>
#include <velocypack/Value.h>

#include <Basics/StaticStrings.h>
#include <Basics/StringUtils.h>
#include <Basics/VelocyPackHelper.h>
#include <Basics/debugging.h>
#include <Inspection/VPack.h>

#include <chrono>
#include <utility>
#include <fmt/core.h>

namespace arangodb::replication2 {

void TermIndexPair::toVelocyPack(velocypack::Builder& builder) const {
  serialize(builder, *this);
}

auto TermIndexPair::fromVelocyPack(velocypack::Slice slice) -> TermIndexPair {
  return deserialize<TermIndexPair>(slice);
}

TermIndexPair::TermIndexPair(LogTerm term, LogIndex index) noexcept
    : term(term), index(index) {
  // Index 0 has always term 0, and it is the only index with that term.
  // FIXME this should be an if and only if
  TRI_ASSERT((index != LogIndex{0}) || (term == LogTerm{0}));
}

auto operator<<(std::ostream& os, TermIndexPair pair) -> std::ostream& {
  return os << '(' << pair.term << ':' << pair.index << ')';
}

auto to_string(TermIndexPair pair) -> std::string {
  std::stringstream ss;
  ss << pair;
  return ss.str();
}

}  // namespace arangodb::replication2
