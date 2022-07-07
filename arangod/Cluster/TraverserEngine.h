////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <unordered_map>

#include "Aql/Collections.h"
#include "Basics/Common.h"
#include "Graph/Providers/SingleServerProvider.h"
#include "Graph/Steps/SingleServerProviderStep.h"

struct TRI_vocbase_t;

namespace arangodb {

class Result;

namespace transaction {
class Methods;
class Context;
}  // namespace transaction

namespace aql {
class Collections;
class QueryContext;
class VariableGenerator;
}  // namespace aql

namespace graph {
struct BaseOptions;
class EdgeCursor;
struct ShortestPathOptions;
}  // namespace graph

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack

namespace traverser {
struct TraverserOptions;

class BaseEngine {
 public:
  enum EngineType { TRAVERSER, SHORTESTPATH };

  static std::unique_ptr<BaseEngine> BuildEngine(
      TRI_vocbase_t& vocbase, aql::QueryContext& query,
      arangodb::velocypack::Slice info);

  BaseEngine(TRI_vocbase_t& vocbase, aql::QueryContext& query,
             arangodb::velocypack::Slice info);

 public:
  virtual ~BaseEngine();

  // The engine is NOT copyable.
  BaseEngine(BaseEngine const&) = delete;

  void getVertexData(arangodb::velocypack::Slice vertex,
                     arangodb::velocypack::Builder& builder, bool nestedOutput);

  std::shared_ptr<transaction::Context> context() const;

  virtual EngineType getType() const = 0;

  virtual bool produceVertices() const { return true; }

  arangodb::aql::EngineId engineId() const noexcept { return _engineId; }

  virtual graph::BaseOptions& options() = 0;

 protected:
  std::pair<std::vector<graph::IndexAccessor>,
            std::unordered_map<uint64_t, std::vector<graph::IndexAccessor>>>
  parseIndexAccessors(arangodb::velocypack::Slice info, bool lastInFirstOut) const;


  graph::SingleServerBaseProviderOptions produceProviderOptions(
      arangodb::velocypack::Slice info, bool lastInFirstOut);

 protected:
  arangodb::aql::EngineId const _engineId;
  arangodb::aql::QueryContext& _query;
  std::unique_ptr<transaction::Methods> _trx;
  std::unordered_map<std::string, std::vector<std::string>> _vertexShards;
};

class BaseTraverserEngine : public BaseEngine {
 public:
  // Only the Registry (friend) is allowed
  // to create and destroy engines.
  // We can get into undefined state if sth.
  // deletes an engine but the registry
  // does not get informed properly

  BaseTraverserEngine(TRI_vocbase_t& vocbase, aql::QueryContext& query,
                      arangodb::velocypack::Slice info);

  ~BaseTraverserEngine();

  virtual void smartSearch(arangodb::velocypack::Slice,
                           arangodb::velocypack::Builder&) = 0;

  virtual void smartSearchUnified(arangodb::velocypack::Slice,
                                  arangodb::velocypack::Builder&) = 0;

  EngineType getType() const override { return TRAVERSER; }

  bool produceVertices() const override;

  // Inject all variables from VPack information
  void injectVariables(arangodb::velocypack::Slice variables);

  graph::BaseOptions& options() override;

 protected:
  aql::VariableGenerator const* variables() const;

 protected:
  std::unique_ptr<traverser::TraverserOptions> _opts;
  aql::VariableGenerator const* _variables;
};

class ShortestPathEngine : public BaseEngine {
 public:
  // Only the Registry (friend) is allowed
  // to create and destroy engines.
  // We can get into undefined state if sth.
  // deletes an engine but the registry
  // does not get informed properly

  ShortestPathEngine(TRI_vocbase_t& vocbase, aql::QueryContext& query,
                     arangodb::velocypack::Slice info);

  ~ShortestPathEngine();

  void getEdges(arangodb::velocypack::Slice, bool backward,
                arangodb::velocypack::Builder&);

  EngineType getType() const override { return SHORTESTPATH; }

  graph::BaseOptions& options() override;

 private:
  std::pair<std::vector<graph::IndexAccessor>,
            std::unordered_map<uint64_t, std::vector<graph::IndexAccessor>>>
  parseReverseIndexAccessors(arangodb::velocypack::Slice info,
                             bool lastInFirstOut) const;

  arangodb::graph::SingleServerBaseProviderOptions
  produceReverseProviderOptions(arangodb::velocypack::Slice info,
                                bool lastInFirstOut);

 protected:
  std::unique_ptr<graph::ShortestPathOptions> _opts;

  graph::SingleServerProvider<arangodb::graph::SingleServerProviderStep> _forwardProvider;
  graph::SingleServerProvider<arangodb::graph::SingleServerProviderStep> _backwardProvider;
};

class TraverserEngine : public BaseTraverserEngine {
 public:
  // Only the Registry (friend) is allowed
  // to create and destroy engines.
  // We can get into undefined state if sth.
  // deletes an engine but the registry
  // does not get informed properly

  TraverserEngine(TRI_vocbase_t& vocbase, aql::QueryContext& query,
                  arangodb::velocypack::Slice info);

  ~TraverserEngine();

  void getEdges(arangodb::velocypack::Slice, size_t,
                arangodb::velocypack::Builder&);

  void smartSearch(arangodb::velocypack::Slice,
                   arangodb::velocypack::Builder&) override;

  void smartSearchUnified(arangodb::velocypack::Slice,
                          arangodb::velocypack::Builder&) override;

 private:
  graph::SingleServerProvider<arangodb::graph::SingleServerProviderStep> _provider;
};

}  // namespace traverser
}  // namespace arangodb
