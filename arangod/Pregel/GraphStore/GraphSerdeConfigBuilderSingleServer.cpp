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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "Pregel/GraphStore/GraphSerdeConfigBuilderSingleServer.h"

#include "Containers/Enumerate.h"
#include "VocBase/LogicalCollection.h"

namespace arangodb::pregel {

GraphSerdeConfigBuilderSingleServer::GraphSerdeConfigBuilderSingleServer(
    TRI_vocbase_t& vocbase, GraphByCollections const& graphByCollections)
    : vocbase(vocbase), graphByCollections(graphByCollections) {}

[[nodiscard]] auto
GraphSerdeConfigBuilderSingleServer::edgeCollectionRestrictionsByShard() const
    -> ShardMap {
  return graphByCollections.edgeCollectionRestrictions;
}

[[nodiscard]] auto GraphSerdeConfigBuilderSingleServer::checkVertexCollections()
    const -> Result {
  for (std::string const& name : graphByCollections.vertexCollections) {
    auto coll = vocbase.lookupCollection(name);

    if (coll == nullptr || coll->deleted()) {
      return Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name};
    }
  }
  return Result{};
}

[[nodiscard]] auto GraphSerdeConfigBuilderSingleServer::checkEdgeCollections()
    const -> errors::ErrorT<Result, std::vector<CollectionID>> {
  std::vector<CollectionID> result;

  for (std::string const& name : graphByCollections.edgeCollections) {
    auto coll = vocbase.lookupCollection(name);

    if (coll == nullptr || coll->deleted()) {
      return errors::ErrorT<Result, std::vector<CollectionID>>::error(
          Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, name});
    }
    std::vector<std::string> actual = coll->realNamesForRead();
    result.insert(result.end(), actual.begin(), actual.end());
  }

  return errors::ErrorT<Result, std::vector<CollectionID>>::ok(result);
}

[[nodiscard]] auto GraphSerdeConfigBuilderSingleServer::loadableVertexShards()
    const -> LoadableVertexShards {
  auto result = LoadableVertexShards{};

  for (auto const [idx, vertexCollection] :
       enumerate(graphByCollections.vertexCollections)) {
    auto loadableVertexShard =
        LoadableVertexShard{.pregelShard = PregelShard(idx),
                            .vertexShard = vertexCollection,
                            .collectionName = vertexCollection,
                            .edgeShards = {}};
    for (auto const& edgeCollection : graphByCollections.edgeCollections) {
      if (not graphByCollections.isRestricted(vertexCollection,
                                              edgeCollection)) {
        loadableVertexShard.edgeShards.emplace_back(edgeCollection);
      }
    }
    result.loadableVertexShards.emplace_back(loadableVertexShard);
  }
  return result;
}

[[nodiscard]] auto GraphSerdeConfigBuilderSingleServer::responsibleServerMap(
    LoadableVertexShards const& loadableVertexShards) const
    -> ResponsibleServerMap {
  auto result = ResponsibleServerMap{};

  result.responsibleServerMap.resize(
      loadableVertexShards.loadableVertexShards.size(), "");
  return result;
}

}  // namespace arangodb::pregel
