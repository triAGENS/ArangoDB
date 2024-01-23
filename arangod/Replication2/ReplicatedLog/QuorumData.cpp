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

#include "QuorumData.h"

#include <Basics/StaticStrings.h>
#include <velocypack/Iterator.h>
#include <velocypack/Builder.h>

namespace arangodb::replication2::replicated_log {

QuorumData::QuorumData(LogIndex index, LogTerm term,
                       std::vector<ParticipantId> quorum)
    : index(index), term(term), quorum(std::move(quorum)) {}

QuorumData::QuorumData(LogIndex index, LogTerm term)
    : QuorumData(index, term, {}) {}

QuorumData::QuorumData(VPackSlice slice) {
  index = slice.get(StaticStrings::Index).extract<LogIndex>();
  term = slice.get(StaticStrings::Term).extract<LogTerm>();
  for (auto part : VPackArrayIterator(slice.get("quorum"))) {
    quorum.push_back(part.copyString());
  }
}

void QuorumData::toVelocyPack(velocypack::Builder& builder) const {
  VPackObjectBuilder ob(&builder);
  builder.add(StaticStrings::Index, VPackValue(index.value));
  builder.add(StaticStrings::Term, VPackValue(term.value));
  {
    VPackArrayBuilder ab(&builder, "quorum");
    for (auto const& part : quorum) {
      builder.add(VPackValue(part));
    }
  }
}

}  // namespace arangodb::replication2::replicated_log
