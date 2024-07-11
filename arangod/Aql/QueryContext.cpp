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

#include "QueryContext.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Ast.h"
#include "Basics/debugging.h"
#include "Basics/Exceptions.h"
#include "Basics/GlobalResourceMonitor.h"
#include "Basics/StaticStrings.h"
#include "Basics/system-functions.h"
#include "Cluster/ServerState.h"
#include "ClusterEngine/ClusterEngine.h"
#include "Graph/Graph.h"
#include "Graph/GraphManager.h"
#include "Logger/LogMacros.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "VocBase/LogicalDataSource.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

using namespace arangodb;
using namespace arangodb::aql;

/// @brief creates a query
QueryContext::QueryContext(TRI_vocbase_t& vocbase,
                           transaction::OperationOrigin operationOrigin,
                           QueryId id)
    : _resourceMonitor(
          std::make_shared<ResourceMonitor>(GlobalResourceMonitor::instance())),
      _queryId(id ? id : TRI_NewServerSpecificTick()),
      _collections(&vocbase),
      _vocbase(vocbase),
      _execState(QueryExecutionState::ValueType::INVALID_STATE),
      _operationOrigin(operationOrigin),
      _numRequests(0) {
  // aql analyzers should be able to run even during recovery when AqlFeature
  // is not started. And as optimization - these queries do not need
  // queryRegistry
  if (&_vocbase != &_vocbase.server()
                        .getFeature<DatabaseFeature>()
                        .getCalculationVocbase() &&
      !AqlFeature::lease()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
  }
}

/// @brief destroys a query
QueryContext::~QueryContext() {
  _graphs.clear();
  if (&_vocbase != &_vocbase.server()
                        .getFeature<DatabaseFeature>()
                        .getCalculationVocbase()) {
    AqlFeature::unlease();
  }
}

TRI_vocbase_t& QueryContext::vocbase() const noexcept { return _vocbase; }

transaction::OperationOrigin QueryContext::operationOrigin() const noexcept {
  return _operationOrigin;
}

Collections& QueryContext::collections() {
  TRI_ASSERT(_execState != QueryExecutionState::ValueType::EXECUTION ||
             ClusterEngine::Mocking);
  return _collections;
}

Collections const& QueryContext::collections() const { return _collections; }

/// @brief return the names of collections used in the query
std::vector<std::string> QueryContext::collectionNames() const {
  return _collections.collectionNames();
}

/// @brief return the user that started the query
std::string const& QueryContext::user() const { return StaticStrings::Empty; }

QueryWarnings& QueryContext::warnings() { return _warnings; }

/// @brief look up a graph either from our cache list or from the _graphs
///        collection
ResultT<graph::Graph const*> QueryContext::lookupGraphByName(
    std::string const& name) {
  TRI_ASSERT(_execState != QueryExecutionState::ValueType::EXECUTION);

  auto it = _graphs.find(name);

  if (it != _graphs.end()) {
    return it->second.get();
  }

  graph::GraphManager graphManager{_vocbase, _operationOrigin};

  auto g = graphManager.lookupGraphByName(name);

  if (g.fail()) {
    return g.result();
  }

  auto graphPtr = g.get().get();

  _graphs.emplace(name, std::move(g.get()));

  // return the raw pointer
  return graphPtr;
}

void QueryContext::addDataSource(  // track DataSource
    std::shared_ptr<arangodb::LogicalDataSource> const&
        ds  // DataSource to track
) {
  TRI_ASSERT(_execState != QueryExecutionState::ValueType::EXECUTION);
  _queryDataSources.try_emplace(ds->guid(), ds->name());
}

aql::Ast* QueryContext::ast() { return _ast.get(); }

void QueryContext::addLeaseFromRemoteGuard(
    cluster::LeaseManager::LeaseFromRemoteGuard&& guard) {
  _leasesFromRemote.emplace_back(std::move(guard));
}

void QueryContext::addLeaseToRemoteGuard(
    cluster::LeaseManager::LeaseToRemoteGuard&& guard) {
  _leasesToRemote.emplace_back(std::move(guard));
}

void QueryContext::enterV8Executor() {
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_NOT_IMPLEMENTED,
                                 "V8 support not implemented");
}
