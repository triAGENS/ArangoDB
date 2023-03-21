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
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <map>

#include "Basics/ResultT.h"
#include "Cluster/ClusterTypes.h"
#include "Inspection/Format.h"
#include "Inspection/Types.h"
#include "Pregel/ExecutionNumber.h"
#include "Pregel/Status/Status.h"
#include "Pregel/Utils.h"
#include "Pregel/Worker/Messages.h"

namespace arangodb::pregel {

namespace conductor::message {

struct ConductorStart {};
template<typename Inspector>
auto inspect(Inspector& f, ConductorStart& x) {
  return f.object(x).fields();
}

struct WorkerCreated {};
template<typename Inspector>
auto inspect(Inspector& f, WorkerCreated& x) {
  return f.object(x).fields();
}

struct GraphLoaded {
  ExecutionNumber executionNumber;
  uint64_t vertexCount;
  uint64_t edgeCount;
};
template<typename Inspector>
auto inspect(Inspector& f, GraphLoaded& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("vertexCount", x.vertexCount), f.field("edgeCount", x.edgeCount));
}

struct GlobalSuperStepFinished {};
template<typename Inspector>
auto inspect(Inspector& f, GlobalSuperStepFinished& x) {
  return f.object(x).fields();
}

struct ResultCreated {
  ResultT<PregelResults> results = {PregelResults{}};
};
template<typename Inspector>
auto inspect(Inspector& f, ResultCreated& x) {
  return f.object(x).fields(f.field("results", x.results));
}

struct StatusUpdate {
  ExecutionNumber executionNumber;
  Status status;
};
template<typename Inspector>
auto inspect(Inspector& f, StatusUpdate& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("status", x.status));
}
struct ConductorMessages
    : std::variant<ConductorStart, ResultT<WorkerCreated>, ResultT<GraphLoaded>,
                   ResultT<GlobalSuperStepFinished>, ResultCreated,
                   StatusUpdate> {
  using std::variant<ConductorStart, ResultT<WorkerCreated>,
                     ResultT<GraphLoaded>, ResultT<GlobalSuperStepFinished>,
                     ResultCreated, StatusUpdate>::variant;
};
template<typename Inspector>
auto inspect(Inspector& f, ConductorMessages& x) {
  return f.variant(x).unqualified().alternatives(
      arangodb::inspection::type<ConductorStart>("Start"),
      arangodb::inspection::type<ResultT<WorkerCreated>>("WorkerCreated"),
      arangodb::inspection::type<ResultT<GraphLoaded>>("GraphLoaded"),
      arangodb::inspection::type<ResultT<GlobalSuperStepFinished>>(
          "GlobalSuperStepFinished"),
      arangodb::inspection::type<ResultCreated>("ResultCreated"),
      arangodb::inspection::type<StatusUpdate>("StatusUpdate"));
}

}  // namespace conductor::message

struct PrepareGlobalSuperStep {
  ExecutionNumber executionNumber;
  uint64_t gss;
  uint64_t vertexCount;
  uint64_t edgeCount;
};
template<typename Inspector>
auto inspect(Inspector& f, PrepareGlobalSuperStep& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field(Utils::globalSuperstepKey, x.gss),
      f.field("vertexCount", x.vertexCount), f.field("edgeCount", x.edgeCount));
}

struct RunGlobalSuperStep {
  ExecutionNumber executionNumber;
  uint64_t gss;
  uint64_t vertexCount;
  uint64_t edgeCount;
  VPackBuilder aggregators;
};

template<typename Inspector>
auto inspect(Inspector& f, RunGlobalSuperStep& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field(Utils::globalSuperstepKey, x.gss),
      f.field("vertexCount", x.vertexCount), f.field("edgeCount", x.edgeCount),
      f.field("aggregators", x.aggregators));
}

// TODO split into Store and Cleanup
struct FinalizeExecution {
  ExecutionNumber executionNumber;
  bool store;
};
template<typename Inspector>
auto inspect(Inspector& f, FinalizeExecution& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("store", x.store));
}

struct CollectPregelResults {
  ExecutionNumber executionNumber;
  bool withId;
};
template<typename Inspector>
auto inspect(Inspector& f, CollectPregelResults& x) {
  return f.object(x).fields(
      f.field(Utils::executionNumberKey, x.executionNumber),
      f.field("withId", x.withId).fallback(false));
}

}  // namespace arangodb::pregel

template<>
struct fmt::formatter<arangodb::pregel::PrepareGlobalSuperStep>
    : arangodb::inspection::inspection_formatter {};
template<>
struct fmt::formatter<arangodb::pregel::RunGlobalSuperStep>
    : arangodb::inspection::inspection_formatter {};
