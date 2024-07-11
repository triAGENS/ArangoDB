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

#include "Aql/QueryOptions.h"
#include "Scheduler/SchedulerFeature.h"

#include <memory>

struct TRI_vocbase_t;

namespace arangodb {

namespace velocypack {
class Builder;
}

class Scheduler;

namespace transaction {
struct OperationOrigin;
}

namespace aql {

class QueryString;
struct QueryResult;
[[nodiscard]] auto runStandaloneAqlQuery(
    TRI_vocbase_t& vocbase, transaction::OperationOrigin origin,
    QueryString query, std::shared_ptr<velocypack::Builder> bindVars,
    QueryOptions options = {},
    Scheduler* scheduler = SchedulerFeature::SCHEDULER)
    -> futures::Future<QueryResult>;
}  // namespace aql
}  // namespace arangodb