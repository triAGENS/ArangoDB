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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Replication2/ReplicatedState/ReplicatedStateTraits.h"
#include "Replication2/Streams/StreamSpecification.h"

#include "Basics/overload.h"
#include "Inspection/VPack.h"
#include "Inspection/VPackLoadInspector.h"

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace arangodb::replication2::replicated_state {
namespace prototype {

// PrototypeLogEntry fields
inline constexpr std::string_view kOp = "op";
inline constexpr std::string_view kType = "type";

// Operation names
inline constexpr std::string_view kDelete = "delete";
inline constexpr std::string_view kInsert = "insert";
inline constexpr std::string_view kBulkDelete = "bulkDelete";

struct PrototypeLogEntry {
  struct InsertOperation {
    std::unordered_map<std::string, std::string> map;
  };
  struct DeleteOperation {
    std::string key;
  };
  struct BulkDeleteOperation {
    std::vector<std::string> keys;
  };

  std::variant<DeleteOperation, InsertOperation, BulkDeleteOperation> op;

  auto getType() noexcept -> std::string_view;
};

template<class Inspector>
auto inspect(Inspector& f, PrototypeLogEntry::InsertOperation& x) {
  return f.object(x).fields(f.field("map", x.map));
}

template<class Inspector>
auto inspect(Inspector& f, PrototypeLogEntry::DeleteOperation& x) {
  return f.object(x).fields(f.field("key", x.key));
}

template<class Inspector>
auto inspect(Inspector& f, PrototypeLogEntry::BulkDeleteOperation& x) {
  return f.object(x).fields(f.field("keys", x.keys));
}

template<class Inspector>
auto inspect(Inspector& f, PrototypeLogEntry& x) {
  if constexpr (Inspector::isLoading) {
    auto typeSlice = f.slice().get(kType);
    TRI_ASSERT(typeSlice.isString());

    auto opLoader = [&]<class T>(T op) {
      auto opSlice = f.slice().get(kOp);
      Inspector ff(opSlice, {});
      auto res = ff.apply(op);
      if (res.ok()) {
        x.op = op;
      }
      return res;
    };

    if (typeSlice.isEqualString(kInsert)) {
      return opLoader(PrototypeLogEntry::InsertOperation{});
    } else if (typeSlice.isEqualString(kDelete)) {
      return opLoader(PrototypeLogEntry::DeleteOperation{});
    } else if (typeSlice.isEqualString(kBulkDelete)) {
      return opLoader(PrototypeLogEntry::BulkDeleteOperation{});
    } else {
    }
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        basics::StringUtils::concatT("Unknown operation '",
                                     typeSlice.copyString(), "'"));
  } else {
    auto& b = f.builder();
    VPackObjectBuilder ob(&b);
    b.add(kType, x.getType());
    b.add(VPackValue(kOp));
    return std::visit([&](auto&& op) { return f.apply(op); }, x.op);
  }
}

}  // namespace prototype

template<>
struct EntryDeserializer<prototype::PrototypeLogEntry> {
  auto operator()(streams::serializer_tag_t<prototype::PrototypeLogEntry>,
                  velocypack::Slice s) const -> prototype::PrototypeLogEntry;
};

template<>
struct EntrySerializer<prototype::PrototypeLogEntry> {
  void operator()(streams::serializer_tag_t<prototype::PrototypeLogEntry>,
                  prototype::PrototypeLogEntry const& e,
                  velocypack::Builder& b) const;
};

}  // namespace arangodb::replication2::replicated_state
