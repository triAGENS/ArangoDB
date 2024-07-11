////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2024-2024 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "LeasesReport.h"

namespace arangodb::cluster {
template<class Inspector>
auto inspect(Inspector& f, LeasesReport& x) {
  return f.object(x).fields(f.field("leasedFromRemote", x.leasesFromRemote),
                            f.field("leasedToRemote", x.leasesToRemote));
}

template<class Inspector>
auto inspect(Inspector& f, ManyServersLeasesReport::EntryOrError& x) {
  return f.variant(x.value).unqualified().alternatives(
      arangodb::inspection::inlineType<LeasesReport>(),
      arangodb::inspection::inlineType<Result>());
}

template<class Inspector>
auto inspect(Inspector& f, ManyServersLeasesReport& x) {
  return f.apply(x.serverLeases);
}

}  // namespace arangodb::cluster
