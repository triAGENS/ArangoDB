////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_VOCBASE_IDENTIFIERS_INDEX_ID_H
#define ARANGOD_VOCBASE_IDENTIFIERS_INDEX_ID_H 1

#include "Basics/Identifier.h"

namespace arangodb {
/// @brief server id type
class IndexId : public arangodb::basics::Identifier {
 public:
  constexpr IndexId() noexcept : Identifier() {}
  constexpr explicit IndexId(BaseType id) noexcept : Identifier(id) {}

 public:
  bool isNone() const { return id() == std::numeric_limits<BaseType>::max(); }
  bool isPrimary() const { return id() == 0; }
  bool isEdge() const { return id() == 1 || id() == 2; }

 public:
  /// @brief create an invalid index id
  static constexpr IndexId none() {
    return IndexId{std::numeric_limits<BaseType>::max()};
  }

  /// @brief create an id for a primary index
  static constexpr IndexId primary() { return IndexId{0}; }

  /// @brief create an id for an edge _from index (rocksdb)
  static constexpr IndexId edgeFrom() { return IndexId{1}; }

  /// @brief create an id for an edge _to index (rocksdb)
  static constexpr IndexId edgeTo() { return IndexId{2}; }

  /// @brief create an id for an edge index (mmfiles)
  static constexpr IndexId edge() { return IndexId{1}; }
};

}  // namespace arangodb

DECLARE_HASH_FOR_IDENTIFIER(arangodb::IndexId)

#endif
