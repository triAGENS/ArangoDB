////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#include "SingleProviderPathResult.h"
#include "Basics/StaticStrings.h"

#include "Graph/PathManagement/PathStore.h"
#include "Graph/PathManagement/PathStoreTracer.h"
#include "Graph/Providers/ClusterProvider.h"
#include "Graph/Providers/ProviderTracer.h"
#include "Graph/Providers/SingleServerProvider.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::graph;

template <class ProviderType, class PathStoreType, class Step>
SingleProviderPathResult<ProviderType, PathStoreType, Step>::SingleProviderPathResult(
    Step* step, ProviderType& provider, PathStoreType& store)
    : _step(step), _provider(provider), _store(store) {
  TRI_ASSERT(_step != nullptr);
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::clear() -> void {
  _vertices.clear();
  _edges.clear();
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::appendVertex(typename Step::Vertex v)
    -> void {
  _vertices.push_back(std::move(v));
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::prependVertex(typename Step::Vertex v)
    -> void {
  _vertices.insert(_vertices.begin(), std::move(v));
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::appendEdge(typename Step::Edge e)
    -> void {
  _edges.push_back(std::move(e));
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::prependEdge(typename Step::Edge e)
    -> void {
  _edges.insert(_edges.begin(), std::move(e));
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::toVelocyPack(
    arangodb::velocypack::Builder& builder) -> void {
  if (_vertices.empty()) {
    _store.visitReversePath(*_step, [&](Step const& s) -> bool {
      prependVertex(s.getVertex());
      if (s.getEdge().isValid()) {
        prependEdge(s.getEdge());
      }
      return true;
    });
  }
  VPackObjectBuilder path{&builder};
  {
    builder.add(VPackValue(StaticStrings::GraphQueryVertices));
    VPackArrayBuilder vertices{&builder};
    // Write first part of the Path
    for (size_t i = 0; i < _vertices.size(); i++) {
      _provider.addVertexToBuilder(_vertices[i], builder);
    }
  }

  {
    builder.add(VPackValue(StaticStrings::GraphQueryEdges));
    VPackArrayBuilder edges(&builder);
    // Write first part of the Path
    for (size_t i = 0; i < _edges.size(); i++) {
      _provider.addEdgeToBuilder(_edges[i], builder);
    }
  }
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::writeSmartGraphDFSResult(
    arangodb::velocypack::Builder& result, size_t& currentLength) -> void {
  size_t prevIndex = 0;

  auto writeDFSStepToBuilder = [&](Step& step) {
    VPackArrayBuilder arrayGuard(&result);
    // Create a VPackValue based on the char* inside the HashedStringRef, will save us a String copy each time.
    result.add(VPackValue(step.getVertex().getID().begin()));

    // Index position of previous step
    result.add(VPackValue(prevIndex));
    // The depth of the current step
    result.add(VPackValue(step.getDepth()));

    bool isResponsible = step.isResponsible(_provider.trx());
    // Print if we have a loose end here (looseEnd := this server is NOT responsible)
    result.add(VPackValue(!isResponsible));
    if (isResponsible) {
      // This server needs to provide the data for the vertex
      _provider.addVertexToBuilder(step.getVertex(), result);
    } else {
      result.add(VPackSlice::nullSlice());
    }

    _provider.addEdgeToBuilder(step.getEdge(), result);
  };  // TODO: Create method instead of lambda

  std::vector<Step*> toWrite{};

  _store.modifyReversePath(*_step, [&](Step& step) -> bool {
    if (!step.hasLocalSchreierIndex()) {
      toWrite.emplace_back(&step);
      return true;
    } else {
      prevIndex = step.getLocalSchreierIndex();
      return false;
    }
  });

  for (auto it = toWrite.rbegin(); it != toWrite.rend(); it++) {
    TRI_ASSERT(!(*it)->hasLocalSchreierIndex());
    writeDFSStepToBuilder(**it);

    prevIndex = currentLength;
    (*it)->setLocalSchreierIndex(currentLength++);
  }
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::writeSmartGraphBFSResult(
    std::unordered_map<VertexType, std::vector<std::unique_ptr<PathResultInterface>>>& bfsLookupTable,
    size_t& bfsCurrentDepth) -> void {
  if constexpr (std::is_same_v<Step, ClusterProvider::Step>) {
    // TODO: before removing this, check why we're compiling SingleProviderPathResult for ClusterProvider
    TRI_ASSERT(false);
    return;
  }

  auto writeStepIntoLookupTable = [&](Step* step) {
    // link step to previous
    VertexRef previousEntry =
        _store.getStepReference(_step->getPrevious()).getVertexIdentifier();
    TRI_ASSERT(bfsLookupTable.find(previousEntry) != bfsLookupTable.end());
    bfsLookupTable.at(previousEntry).emplace_back(_step);
    LOG_DEVEL << "Wrote step " << _step->getVertexIdentifier() << " into bfsLookupTable";

    // TODO: create additional entry for step itself // TODO MIssing right now
  };

  auto writeDepthIntoBuilder = [&](size_t depth) {
    VPackArrayBuilder arrayGuard(&result);
    // 1.) Write current depth
    result.add(VPackValue(bfsCurrentDepth));

    for (auto const& entry : bfsLookupTable) {
      VertexRef vertexName = entry.first;

      auto relatedSteps = entry.second;
      TRI_ASSERT(relatedSteps.size() > 0);

      // 2.) Write Vertex itself into Builder
      // we can use at(0) because any relation points to our origin vertex, we just need at least one entry
      auto vertexStep =
          _store.getStepReference(relatedSteps.at(0).get()._step->getPrevious());
      _provider.addVertexToBuilder(vertexStep.getVertex(), result);

      // Now iterate through all collected steps and write them into the result builder
      // Format is:
      // I. Edge Content itself
      // II. cursorID (clarify) // TODO: CHECK IMPORTANT CURSOR ID
      // III. nextVertexID (our related step vertex id)
      for (Step* step : relatedSteps) {
        _provider.addEdgeToBuilder(step->getEdge(), result);
        result.add(VPackValue(0));  // TODO: CHECK IMPORTANT CURSOR ID
        result.add(VPackValue(step->getVertexIdentifier().begin()));
      }
    }
  };

  // OPEN TODO: currently only the first step is beeing proper initialized. This is missing right now.

  LOG_DEVEL << " == HANDLING STEP == " << _step->getVertexIdentifier().begin();
  if (_step->isFirst()) {
    LOG_DEVEL << "handled first step";
    TRI_ASSERT(bfsCurrentDepth == 0);
    bfsLookupTable.insert({_step->getVertexIdentifier(), {}});
  } else {
    // means we're not in initial step
    LOG_DEVEL << "=> Step depth: " << _step->getDepth() << " ,iteration: " << bfsCurrentDepth;

    // get the location of previous entry
    if (_step->getDepth() - bfsCurrentDepth == 1) {
      // B, C and D got written
      writeStepIntoLookupTable(_step);
    } else {
      LOG_DEVEL << "finalizing - will write into builder";
      // we finalized a depth of range [0, 1] -> this means we can write a complete depth set into the builder
      writeDepthIntoBuilder(bfsCurrentDepth);

      // additionally, this means we'll start a new depth here (increase depth information)
      bfsCurrentDepth++;

      // We do not need old information which is stored in our _bfsLookupTable (clear now).
      bfsLookupTable.clear();
      writeStepIntoLookupTable(_step);
    }
  }
}

template <class ProviderType, class PathStoreType, class Step>
auto SingleProviderPathResult<ProviderType, PathStoreType, Step>::isEmpty() const
    -> bool {
  return false;
}

/* SingleServerProvider Section */

template class ::arangodb::graph::SingleProviderPathResult<
    ::arangodb::graph::SingleServerProvider, ::arangodb::graph::PathStore<::arangodb::graph::SingleServerProvider::Step>,
    ::arangodb::graph::SingleServerProvider::Step>;

template class ::arangodb::graph::SingleProviderPathResult<
    ::arangodb::graph::ProviderTracer<::arangodb::graph::SingleServerProvider>,
    ::arangodb::graph::PathStoreTracer<::arangodb::graph::PathStore<::arangodb::graph::SingleServerProvider::Step>>,
    ::arangodb::graph::SingleServerProvider::Step>;

/* ClusterProvider Section */
template class ::arangodb::graph::SingleProviderPathResult<
    ::arangodb::graph::ClusterProvider, ::arangodb::graph::PathStore<::arangodb::graph::ClusterProvider::Step>,
    ::arangodb::graph::ClusterProvider::Step>;

template class ::arangodb::graph::SingleProviderPathResult<
    ::arangodb::graph::ProviderTracer<::arangodb::graph::ClusterProvider>,
    ::arangodb::graph::PathStoreTracer<::arangodb::graph::PathStore<::arangodb::graph::ClusterProvider::Step>>,
    ::arangodb::graph::ClusterProvider::Step>;