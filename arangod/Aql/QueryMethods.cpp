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

#include "QueryMethods.h"

#include "Aql/QueryAborter.h"
#include "Aql/QueryString.h"
#include "Transaction/OperationOrigin.h"
#include "Query.h"
#include "Transaction/StandaloneContext.h"

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::transaction;

namespace arangodb::aql {
auto runStandaloneAqlQuery(TRI_vocbase_t& vocbase,
                           transaction::OperationOrigin origin,
                           QueryString queryString,
                           std::shared_ptr<velocypack::Builder> bindVars,
                           QueryOptions options, Scheduler* scheduler)
    -> futures::Future<QueryResult> {
  auto query = arangodb::aql::Query::create(
      transaction::StandaloneContext::create(vocbase, origin),
      std::move(queryString), std::move(bindVars), options, scheduler);

  auto aborter = std::make_shared<aql::QueryAborter>(query);

  // TODO: Make this non-blocking and erase executeSync from the API
  // This Promise here is just to turn a sync API into a future.
  // As soon as query can expose a future this is not necessary anymore
  auto promise = futures::Promise<QueryResult>{};
  auto future = promise.getFuture();
  aql::QueryResult queryResult = query->executeSync(aborter);
  promise.setValue(std::move(queryResult));
  return future;
}
}  // namespace arangodb::aql
