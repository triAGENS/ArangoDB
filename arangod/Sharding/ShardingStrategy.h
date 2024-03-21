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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Common.h"
#include "Cluster/Utils/ShardID.h"

#include <functional>
#include <string>
#include <string_view>
#include <memory>

namespace arangodb {
class ShardingInfo;

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack

class ShardingStrategy {
  ShardingStrategy(ShardingStrategy const&) = delete;
  ShardingStrategy& operator=(ShardingStrategy const&) = delete;

 public:
  typedef std::function<std::unique_ptr<ShardingStrategy>(ShardingInfo*)>
      FactoryFunction;

  ShardingStrategy() = default;
  virtual ~ShardingStrategy() = default;

  virtual bool isCompatible(ShardingStrategy const* other) const;

  virtual std::string const& name() const = 0;

  virtual bool usesDefaultShardKeys() const noexcept = 0;

  virtual void toVelocyPack(velocypack::Builder& result) const;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief find the shard that is responsible for a document, which is given
  /// as a VelocyPackSlice.
  ///
  /// There are two modes, one assumes that the document is given as a
  /// whole (`docComplete`==`true`), in this case, the non-existence of
  /// values for some of the sharding attributes is silently ignored
  /// and treated as if these values were `null`. In the second mode
  /// (`docComplete`==false) leads to an error which is reported by
  /// returning TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, which is the only
  /// error code that can be returned.
  ///
  /// In either case, if the collection is found, the variable
  /// shardID is set to the ID of the responsible shard and the flag
  /// `usesDefaultShardingAttributes` is used set to `true` if and only if
  /// `_key` is the one and only sharding attribute.
  ////////////////////////////////////////////////////////////////////////////////

  virtual ResultT<ShardID> getResponsibleShard(velocypack::Slice slice,
                                               bool docComplete,
                                               bool& usesDefaultShardKeys,
                                               std::string_view key) = 0;
};

}  // namespace arangodb
