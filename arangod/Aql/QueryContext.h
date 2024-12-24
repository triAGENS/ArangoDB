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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Aql/Collections.h"
#include "Aql/QueryExecutionState.h"
#include "Aql/QueryOptions.h"
#include "Aql/QueryWarnings.h"
#include "Aql/types.h"
#include "Basics/ResourceUsage.h"
#include "Basics/ResultT.h"
#include "Transaction/OperationOrigin.h"
#include "VocBase/voc-types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

struct TRI_vocbase_t;

namespace arangodb {
class CollectionNameResolver;
class LogicalDataSource;  // forward declaration

namespace transaction {
class Context;
class Methods;
}  // namespace transaction

namespace velocypack {
struct Options;
}

namespace graph {
class Graph;
}

namespace aql {

class Ast;

/// @brief an AQL query basic interface
class QueryContext {
 private:
  QueryContext(QueryContext const&) = delete;
  QueryContext& operator=(QueryContext const&) = delete;

 public:
  explicit QueryContext(TRI_vocbase_t& vocbase,
                        transaction::OperationOrigin operationOrigin,
                        QueryId id = 0);

  virtual ~QueryContext();

  ResourceMonitor& resourceMonitor() noexcept { return *_resourceMonitor; }

  ResourceMonitor const& resourceMonitor() const noexcept {
    return *_resourceMonitor;
  }
  std::shared_ptr<ResourceMonitor> resourceMonitorAsSharedPtr() noexcept {
    return _resourceMonitor;
  }
  std::shared_ptr<ResourceMonitor const> resourceMonitorAsSharedPtr()
      const noexcept {
    return _resourceMonitor;
  }

  /// @brief get the vocbase
  TRI_vocbase_t& vocbase() const noexcept;

  transaction::OperationOrigin operationOrigin() const noexcept;

  Collections& collections();
  Collections const& collections() const;

  /// @brief return the names of collections used in the query
  std::vector<std::string> collectionNames() const;

  /// @brief return the user that started the query
  virtual std::string const& user() const;

  /// warnings access is thread safe
  QueryWarnings& warnings();

  /// warnings access is thread safe
  QueryWarnings const& warnings() const;

  /// @brief look up a graph in the _graphs collection
  ResultT<graph::Graph const*> lookupGraphByName(std::string const& name);

  /// @brief note that the query uses the DataSource
  void addDataSource(LogicalDataSource const& ds);

  QueryExecutionState::ValueType state() const noexcept { return _execState; }

  TRI_voc_tick_t id() const noexcept { return _queryId; }

  aql::Ast* ast() const;

  /// @brief Acquire a lock_guard on the mutex to serialize concurrent snippet
  /// execution
  std::lock_guard<std::mutex> acquireLockGuard() {
    return std::lock_guard{_mutex};
  }

  void incHttpRequests(unsigned i) noexcept {
    _numRequests.fetch_add(i, std::memory_order_relaxed);
  }

  virtual QueryOptions const& queryOptions() const = 0;

  virtual QueryOptions& queryOptions() noexcept = 0;

  /// @brief pass-thru a resolver object from the transaction context
  virtual CollectionNameResolver const& resolver() const = 0;

  virtual velocypack::Options const& vpackOptions() const = 0;

  /// @brief create a transaction::Context
  virtual std::shared_ptr<transaction::Context> newTrxContext() const = 0;

  virtual transaction::Methods& trxForOptimization() = 0;

  virtual bool killed() const = 0;

  // Debug method to kill a query at a specific position
  // during execution. It internally asserts that the query
  // is actually visible through other APIS (e.g. current queries)
  // so user actually has a chance to kill it here.
  virtual void debugKillQuery() = 0;

  /// @brief whether or not a query is a modification query
  virtual bool isModificationQuery() const noexcept = 0;
  virtual bool isAsyncQuery() const noexcept = 0;

  virtual double getLockTimeout() const noexcept = 0;
  virtual void setLockTimeout(double timeout) = 0;

  virtual void enterV8Executor();

  virtual void exitV8Executor() {}

  virtual bool hasEnteredV8Executor() const { return false; }

  // base overhead for each query. the number used here is somewhat arbitrary.
  // it is just that all the basics data structures of a query are not totally
  // free, and there is not other accounting for them. note: this value is
  // counted up in the constructor and counted down in the destructor.
  constexpr static std::size_t baseMemoryUsage = 8192;

  enum class ExecuteCallerWaiting {
    Asynchronously,
    Synchronously,
  };

  ExecuteCallerWaiting executeCallerWaiting() const noexcept;

 protected:
  /// @brief current resources and limits used by query
  std::shared_ptr<ResourceMonitor> _resourceMonitor;

  TRI_voc_tick_t const _queryId;

  /// @brief thread-safe query warnings collector
  QueryWarnings _warnings;

  /// @brief collections used in the query
  Collections _collections;

  /// @brief pointer to vocbase the query runs in
  TRI_vocbase_t& _vocbase;

  /// @brief graphs used in query, identified by name
  std::unordered_map<std::string, std::unique_ptr<graph::Graph>> _graphs;

  /// @brief set of DataSources used in the query
  ///        needed for the query cache, stores datasource guid -> datasource
  ///        name
  std::unordered_map<std::string, std::string> _queryDataSources;

  /// @brief current state the query is in (used for profiling and error
  /// messages)
  std::atomic<QueryExecutionState::ValueType> _execState;

  transaction::OperationOrigin const _operationOrigin;

  /// @brief _ast, we need an ast to manage the memory for AstNodes, even
  /// if we do not have a parser, because AstNodes occur in plans and engines
  std::unique_ptr<Ast> _ast;

  /// @brief number of HTTP requests executed by the query
  std::atomic<unsigned> _numRequests;

  /// @brief this mutex is used to serialize execution of potentially concurrent
  /// snippets as a result of using parallel gather.
  /// In the future we might want to consider using an rwlock instead so that
  /// read-only snippets can actually run concurrently.
  std::mutex _mutex;

  // Whether the caller executes the query synchronously or asynchronously -
  // i.e. is calling Query::executeSync, Query::executeV8 for the former, or
  // Query::execute for the latter.
  // This is used to decide whether certain network requests set skipScheduler.
  ExecuteCallerWaiting _executeCallerWaiting =
      ExecuteCallerWaiting::Asynchronously;
};

}  // namespace aql
}  // namespace arangodb
