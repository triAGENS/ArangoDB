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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include "Cluster/Utils/ShardID.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedState/StateCommon.h"

#include <map>

namespace arangodb::replication2::storage {

// Metadata owned by the state implementation (e.g. DocumentState) - thus no
// specific type here.
struct StateOwnedMetadata {
  // The default slice must be provided by the state implementation, and should
  // not be None by accident. Otherwise, this could easily break
  // deserialization. However, we can't just delete the default constructor or
  // will break the inspector.
  velocypack::SharedSlice slice;
};

struct PersistedStateInfo {
  LogId stateId;  // could be removed
  replicated_state::SnapshotInfo snapshot;
  replicated_state::StateGeneration generation;
  replication2::agency::ImplementationSpec specification;
  StateOwnedMetadata stateOwnedMetadata;
};

template<class Inspector>
auto inspect(Inspector& f, StateOwnedMetadata& x) {
  return f.apply(x.slice);
}
template<class Inspector>
auto inspect(Inspector& f, PersistedStateInfo& x) {
  return f.object(x).fields(
      f.field("stateId", x.stateId), f.field("snapshot", x.snapshot),
      f.field("generation", x.generation),
      f.field("specification", x.specification),
      f.field("stateOwnedMetadata", x.stateOwnedMetadata));
}

}  // namespace arangodb::replication2::storage
