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
#pragma once

#include "Pregel/GraphStore/GraphSerdeConfig.h"

#include "Pregel/GraphStore/GraphByCollections.h"

#include "Basics/ErrorT.h"
#include "Basics/Result.h"
#include "VocBase/vocbase.h"

#include <string>
#include <unordered_map>

namespace arangodb::pregel {
using ShardMap = std::unordered_map<std::string, std::vector<std::string>>;

struct GraphSerdeConfigBuilderBase {
  [[nodiscard]] virtual auto edgeCollectionRestrictionsByShard() const
      -> ShardMap = 0;
  [[nodiscard]] virtual auto checkVertexCollections() const -> Result = 0;
  [[nodiscard]] virtual auto checkEdgeCollections() const
      -> errors::ErrorT<Result, std::vector<CollectionID>> = 0;

  [[nodiscard]] virtual auto loadableVertexShards() const
      -> LoadableVertexShards = 0;
  [[nodiscard]] virtual auto responsibleServerMap(
      LoadableVertexShards const& loadableVertexShards) const
      -> ResponsibleServerMap = 0;

  [[nodiscard]] virtual auto buildConfig() -> GraphSerdeConfig const;

  [[nodiscard]] static auto construct(
      TRI_vocbase_t& vocbase, GraphByCollections const& graphByCollections)
      -> std::unique_ptr<GraphSerdeConfigBuilderBase>;

  virtual ~GraphSerdeConfigBuilderBase() = default;
};

}  // namespace arangodb::pregel
