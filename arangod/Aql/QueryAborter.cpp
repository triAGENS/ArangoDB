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

#include "QueryAborter.h"

#include "Aql/Query.h"
#include "Aql/SharedQueryState.h"

#include "Logger/LogMacros.h"

using namespace arangodb::aql;

void QueryAborter::abort() noexcept {
  bool expected = false;
  if (_wasAborted.compare_exchange_strong(expected, true)) {
    // Only run this for the firs thread that inserts a TRUE value
    LOG_DEVEL << "TRYING TO ABORT QUERY";
    if (auto q = _query.lock(); q != nullptr) {
      // Query is not yet garbage collected, we can use it
      LOG_DEVEL << "Query is still live " << q->id() << " now aborting it";
      q->sharedState()->executeAndWakeup([q]() noexcept {
        try {
          q->kill();
        } catch (...) {
          LOG_DEVEL
              << "Threw an error when trying to kill a query to abort it.";
        }
        return true;
      });
    }
  }
}

bool QueryAborter::wasAborted() const noexcept { return _wasAborted.load(); }