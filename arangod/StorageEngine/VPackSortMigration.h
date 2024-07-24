////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2024 ArangoDB GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
////////////////////////////////////////////////////////////////////////////////

#include "Basics/Result.h"
#include "Basics/async.h"
#include "VocBase/vocbase.h"

namespace arangodb {

// On dbservers, agents and single servers:
Result analyzeVPackIndexSorting(TRI_vocbase_t& vocbase, VPackBuilder& result);

Result migrateVPackIndexSorting(VPackBuilder& result);

// On coordinators:
async<Result> handleVPackSortMigrationTest(TRI_vocbase_t& vocbase,
                                           VPackBuilder& result);

async<Result> handleVPackSortMigrationAction(TRI_vocbase_t& vocbase,
                                             VPackBuilder& result);

}  // namespace arangodb
