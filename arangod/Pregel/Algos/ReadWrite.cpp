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
/// @author Roman Rabinovich
////////////////////////////////////////////////////////////////////////////////

#include "ReadWrite.h"

#include <utility>
#include "Pregel/Aggregator.h"
#include "Pregel/GraphFormat.h"
#include "Pregel/Iterators.h"
#include "Pregel/MasterContext.h"
#include "Pregel/Utils.h"
#include "Pregel/VertexComputation.h"

using namespace arangodb;
using namespace arangodb::pregel;
using namespace arangodb::pregel::algos;

static std::string const simulatedAggregatorName = "simulatedAggregator";

struct ReadWriteWorkerContext : public WorkerContext {
  ReadWriteWorkerContext() = default;
};

ReadWrite::ReadWrite(application_features::ApplicationServer& server,
                   VPackSlice const& userParams)
    : SimpleAlgorithm(server, "readwrite", userParams) {}

struct ReadWriteGraphFormat final : public GraphFormat<V, E> {
  ReadWriteGraphFormat(application_features::ApplicationServer& server,
                      std::string  sourceFieldName,
                      std::string  resultFieldName)
      : GraphFormat(server),
        sourceFieldName{std::move(sourceFieldName)},
        resultFieldName{std::move(resultFieldName)} {}
  std::string const sourceFieldName;
  std::string const resultFieldName;

  void copyVertexData(arangodb::velocypack::Options const&,
                      std::string const& documentId,
                      arangodb::velocypack::Slice document, V& targetPtr,
                      uint64_t& /*vertexIdRange*/) override {
    arangodb::velocypack::Slice val = document.get(sourceFieldName);
    targetPtr = val.isNumber() ? val.getNumber<V>() : 1.0;

  }

  bool buildVertexDocument(arangodb::velocypack::Builder& b,
                           V const* ptr) const override {
    b.add(resultFieldName, arangodb::velocypack::Value(*ptr));
    return true;
  }
};

GraphFormat<V, E>* ReadWrite::inputFormat() const {
    return new ReadWriteGraphFormat(_server, _sourceField, _resultField);
}

struct ReadWriteComputation : public VertexComputation<V, E, V> {
  ReadWriteComputation() = default;
  void compute(MessageIterator<float> const& messages) override {
    V sum;
    for (const float* msg : messages) {
      sum += *msg;
    }
    aggregate<float>(simulatedAggregatorName, sum);
    sendMessageToAllNeighbours(sum);
  }
};

VertexComputation<V, E, V>* ReadWrite::createComputation(
    WorkerConfig const* config) const {
  return new ReadWriteComputation();
}

WorkerContext* ReadWrite::workerContext(VPackSlice userParams) const {
  return new ReadWriteWorkerContext();
}

struct ReadWriteMasterContext : public MasterContext {
  size_t maxGss;
  explicit ReadWriteMasterContext(VPackSlice userParams) {
    if (userParams.hasKey(Utils::maxGSS)) {
      maxGss = userParams.get(Utils::maxGSS).getInt();
    }
  }

  bool postGlobalSuperstep() override { return globalSuperstep() <= maxGss; };
};

MasterContext* ReadWrite::masterContext(VPackSlice userParams) const {
  return new ReadWriteMasterContext(userParams);
}

IAggregator* ReadWrite::aggregator(std::string const& name) const {
  if (name == simulatedAggregatorName) {
    return new MaxAggregator<V>(-1, false);
  }
  return nullptr;
}
