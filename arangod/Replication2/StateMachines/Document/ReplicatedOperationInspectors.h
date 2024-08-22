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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Inspection/Format.h"
#include "Inspection/Status.h"
#include "Inspection/Types.h"
#include "Inspection/VPack.h"
#include "Replication2/StateMachines/Document/ReplicatedOperation.h"

namespace arangodb::replication2::replicated_state::document {

template<class Enum>
struct EnumTypeTransformer {
  using SerializedType = std::underlying_type_t<Enum>;

  arangodb::inspection::Status toSerialized(Enum v,
                                            SerializedType& result) const {
    result = static_cast<SerializedType>(v);
    return {};
  }

  arangodb::inspection::Status fromSerialized(SerializedType const& v,
                                              Enum& result) const {
    result = Enum{v};
    return {};
  }
};

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::DocumentOperation::Options& x) {
  return f.object(x).fields(f.field("refillIndexCaches", x.refillIndexCaches));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::DocumentOperation& x) {
  return f.object(x).fields(f.field("tid", x.tid), f.field("shard", x.shard),
                            f.field("payload", x.payload),
                            f.field("options", x.options),
                            f.field("username", x.userName));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::AbortAllOngoingTrx& x) {
  return f.object(x).fields();
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Commit& x) {
  return f.object(x).fields(f.field("tid", x.tid));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::IntermediateCommit& x) {
  return f.object(x).fields(f.field("tid", x.tid));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Abort& x) {
  return f.object(x).fields(f.field("tid", x.tid));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Truncate& x) {
  return f.object(x).fields(f.field("tid", x.tid), f.field("shard", x.shard));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::CreateShard& x) {
  return f.object(x).fields(
      f.field("shard", x.shard),
      f.field("collectionType", x.collectionType)
          .transformWith(EnumTypeTransformer<TRI_col_type_e>{}),
      f.field("properties", x.properties));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::ModifyShard& x) {
  return f.object(x).fields(f.field("shard", x.shard),
                            f.field("collection", x.collection),
                            f.field("properties", x.properties));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::DropShard& x) {
  return f.object(x).fields(f.field("shard", x.shard));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::CreateIndex& x) {
  return f.object(x).fields(f.field("shard", x.shard),
                            f.field("properties", x.properties));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::DropIndex& x) {
  return f.object(x).fields(f.field("shard", x.shard),
                            f.field("indexId", x.indexId));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Insert& x) {
  return f.object(x).fields(
      f.template embedFields<ReplicatedOperation::DocumentOperation>(x));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Update& x) {
  return f.object(x).fields(
      f.template embedFields<ReplicatedOperation::DocumentOperation>(x));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Replace& x) {
  return f.object(x).fields(
      f.template embedFields<ReplicatedOperation::DocumentOperation>(x));
}

template<class Inspector>
auto inspect(Inspector& f, ReplicatedOperation::Remove& x) {
  return f.object(x).fields(
      f.template embedFields<ReplicatedOperation::DocumentOperation>(x));
}

template<typename Inspector>
auto inspect(Inspector& f, ReplicatedOperation& x) {
  return f.variant(x.operation)
      .embedded("type")
      .alternatives(
          inspection::type<ReplicatedOperation::AbortAllOngoingTrx>(
              "AbortAllOngoingTrx"),
          inspection::type<ReplicatedOperation::Commit>("Commit"),
          inspection::type<ReplicatedOperation::IntermediateCommit>(
              "IntermediateCommit"),
          inspection::type<ReplicatedOperation::Abort>("Abort"),
          inspection::type<ReplicatedOperation::Truncate>("Truncate"),
          inspection::type<ReplicatedOperation::CreateShard>("CreateShard"),
          inspection::type<ReplicatedOperation::ModifyShard>("ModifyShard"),
          inspection::type<ReplicatedOperation::DropShard>("DropShard"),
          inspection::type<ReplicatedOperation::CreateIndex>("CreateIndex"),
          inspection::type<ReplicatedOperation::DropIndex>("DropIndex"),
          inspection::type<ReplicatedOperation::Insert>("Insert"),
          inspection::type<ReplicatedOperation::Update>("Update"),
          inspection::type<ReplicatedOperation::Replace>("Replace"),
          inspection::type<ReplicatedOperation::Remove>("Remove"));
}

template<typename Inspector>
auto inspect(Inspector& f, ReplicatedOperation::UserTransactionOperation& x) {
  return f.variant(x).embedded("type").alternatives(
      inspection::type<ReplicatedOperation::Commit>("Commit"),
      inspection::type<ReplicatedOperation::IntermediateCommit>(
          "IntermediateCommit"),
      inspection::type<ReplicatedOperation::Abort>("Abort"),
      inspection::type<ReplicatedOperation::Truncate>("Truncate"),
      inspection::type<ReplicatedOperation::Insert>("Insert"),
      inspection::type<ReplicatedOperation::Update>("Update"),
      inspection::type<ReplicatedOperation::Replace>("Replace"),
      inspection::type<ReplicatedOperation::Remove>("Remove"));
}
}  // namespace arangodb::replication2::replicated_state::document

template<>
struct fmt::formatter<
    arangodb::replication2::replicated_state::document::ReplicatedOperation>
    : arangodb::inspection::inspection_formatter {};
