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

#include "Cluster/ClusterTypes.h"
#include "Cluster/Utils/ShardID.h"
#include "VocBase/Identifiers/TransactionId.h"
#include "VocBase/Methods/Indexes.h"

#include <variant>

namespace arangodb::replication2::replicated_state::document {

template<typename T, typename... U>
concept IsAnyOf = (std::same_as<T, U> || ...);

/*
 * When a log entry is received, the ReplicatedOperation tells the state machine
 * what it has to do (i.e. start a transaction, drop a shard, ...).
 */
struct ReplicatedOperation {
  ReplicatedOperation() = default;

  struct DocumentOperation {
    TransactionId tid;
    ShardID shard;
    velocypack::SharedSlice payload;
    std::string userName;

    struct Options {
      // Automatically refill in-memory cache entries after
      // inserts/updates/replaces for all indexes that have an in-memory cache
      // attached
      bool refillIndexCaches;
    };
    std::optional<Options> options;

    // TODO: This somehow seems to be needed for Inspection.
    // Would like to remove it again though.
    DocumentOperation() {}

    explicit DocumentOperation(TransactionId tid, ShardID shard,
                               velocypack::SharedSlice payload,
                               std::string_view userName,
                               std::optional<Options> options = std::nullopt);

    friend auto operator==(DocumentOperation const& a,
                           DocumentOperation const& b) -> bool {
      return a.tid == b.tid && a.shard == b.shard &&
             a.payload.binaryEquals(b.payload.slice());
    }
  };

  struct AbortAllOngoingTrx {
    friend auto operator==(AbortAllOngoingTrx const&, AbortAllOngoingTrx const&)
        -> bool = default;
  };

  struct Commit {
    TransactionId tid;

    friend auto operator==(Commit const&, Commit const&) -> bool = default;
  };

  struct IntermediateCommit {
    TransactionId tid;

    friend auto operator==(IntermediateCommit const&, IntermediateCommit const&)
        -> bool = default;
  };

  struct Abort {
    TransactionId tid;

    friend auto operator==(Abort const&, Abort const&) -> bool = default;
  };

  struct Truncate {
    TransactionId tid;
    ShardID shard;
    std::string userName;

    friend auto operator==(Truncate const&, Truncate const&) -> bool = default;
  };

  struct CreateShard {
    ShardID shard;
    TRI_col_type_e collectionType;
    velocypack::SharedSlice properties;

    friend auto operator==(CreateShard const& lhs, CreateShard const& rhs)
        -> bool {
      return lhs.shard == rhs.shard &&
             lhs.collectionType == rhs.collectionType &&
             lhs.properties.binaryEquals(rhs.properties.slice());
    }
  };

  struct ModifyShard {
    ShardID shard;
    CollectionID collection;
    velocypack::SharedSlice properties;

    friend auto operator==(ModifyShard const& lhs, ModifyShard const& rhs)
        -> bool {
      return lhs.shard == rhs.shard && lhs.collection == rhs.collection &&
             lhs.properties.binaryEquals(rhs.properties.slice());
    }
  };

  struct DropShard {
    ShardID shard;

    friend auto operator==(DropShard const&, DropShard const&)
        -> bool = default;
  };

  struct CreateIndex {
    ShardID shard;
    velocypack::SharedSlice properties;

    friend auto operator==(CreateIndex const& lhs, CreateIndex const& rhs)
        -> bool {
      return lhs.shard == rhs.shard &&
             lhs.properties.binaryEquals(rhs.properties.slice());
    }
  };

  struct DropIndex {
    ShardID shard;
    IndexId indexId;

    friend auto operator==(DropIndex const& lhs, DropIndex const& rhs)
        -> bool = default;
  };

  struct Insert : DocumentOperation {
    using DocumentOperation::DocumentOperation;
    Insert(DocumentOperation op) : DocumentOperation(std::move(op)) {}
  };

  struct Update : DocumentOperation {};

  struct Replace : DocumentOperation {};

  struct Remove : DocumentOperation {};

  template<typename Op>
  requires IsAnyOf<std::remove_cvref_t<Op>,                  //
                   ReplicatedOperation::AbortAllOngoingTrx,  //
                   ReplicatedOperation::Commit,              //
                   ReplicatedOperation::IntermediateCommit,  //
                   ReplicatedOperation::Abort,               //
                   ReplicatedOperation::Truncate,            //
                   ReplicatedOperation::CreateShard,         //
                   ReplicatedOperation::ModifyShard,         //
                   ReplicatedOperation::DropShard,           //
                   ReplicatedOperation::CreateIndex,         //
                   ReplicatedOperation::DropIndex,           //
                   ReplicatedOperation::Insert,              //
                   ReplicatedOperation::Update,              //
                   ReplicatedOperation::Replace,             //
                   ReplicatedOperation::Remove>
  explicit ReplicatedOperation(Op operation)
      : ReplicatedOperation(std::in_place, operation) {}

  using OperationType =
      std::variant<AbortAllOngoingTrx, Commit, IntermediateCommit, Abort,
                   Truncate, CreateShard, ModifyShard, DropShard, CreateIndex,
                   DropIndex, Insert, Update, Replace, Remove>;

  OperationType operation;

  using UserTransactionOperation =
      std::variant<Truncate, Insert, Update, Replace, Remove,
                   IntermediateCommit, Commit, Abort>;
  explicit ReplicatedOperation(UserTransactionOperation operation)
      : ReplicatedOperation(std::visit(
            [](auto&& op) {
              return ReplicatedOperation(std::in_place,
                                         std::forward<decltype(op)>(op));
            },
            std::move(operation))) {}

  static auto fromOperationType(OperationType op) noexcept
      -> ReplicatedOperation;
  static auto buildAbortAllOngoingTrxOperation() noexcept
      -> ReplicatedOperation;
  static auto buildCommitOperation(TransactionId tid) noexcept
      -> ReplicatedOperation;
  static auto buildIntermediateCommitOperation(TransactionId tid) noexcept
      -> ReplicatedOperation;
  static auto buildAbortOperation(TransactionId tid) noexcept
      -> ReplicatedOperation;
  static auto buildTruncateOperation(TransactionId tid, ShardID shard,
                                     std::string_view userName) noexcept
      -> ReplicatedOperation;
  static auto buildCreateShardOperation(
      ShardID shard, TRI_col_type_e collectionType,
      velocypack::SharedSlice properties) noexcept -> ReplicatedOperation;
  static auto buildModifyShardOperation(
      ShardID shard, CollectionID collection,
      velocypack::SharedSlice properties) noexcept -> ReplicatedOperation;
  static auto buildDropShardOperation(ShardID shard) noexcept
      -> ReplicatedOperation;
  static auto buildCreateIndexOperation(
      ShardID shard, velocypack::SharedSlice properties) noexcept
      -> ReplicatedOperation;
  static auto buildDropIndexOperation(ShardID shard, IndexId indexId) noexcept
      -> ReplicatedOperation;
  static auto buildDocumentOperation(
      TRI_voc_document_operation_e const& op, TransactionId tid, ShardID shard,
      velocypack::SharedSlice payload, std::string_view userName,
      std::optional<DocumentOperation::Options> options = std::nullopt) noexcept
      -> UserTransactionOperation;

  friend auto operator==(ReplicatedOperation const&, ReplicatedOperation const&)
      -> bool = default;

  friend auto operator==(ReplicatedOperation const&,
                         ReplicatedOperation::OperationType const&) -> bool;
  friend auto operator==(ReplicatedOperation::OperationType const&,
                         ReplicatedOperation const&) -> bool;

 private:
  template<typename... Args>
  explicit ReplicatedOperation(std::in_place_t, Args&&... args) noexcept
      : operation(std::forward<Args>(args)...) {}
};

template<class T>
concept ModifiesUserTransaction = IsAnyOf<std::remove_cvref_t<T>,         //
                                          ReplicatedOperation::Truncate,  //
                                          ReplicatedOperation::Insert,    //
                                          ReplicatedOperation::Update,    //
                                          ReplicatedOperation::Replace,   //
                                          ReplicatedOperation::Remove>;

template<class T>
concept FinishesUserTransaction = IsAnyOf<std::remove_cvref_t<T>,       //
                                          ReplicatedOperation::Commit,  //
                                          ReplicatedOperation::Abort>;

template<class T>
concept FinishesUserTransactionOrIntermediate = FinishesUserTransaction<T> ||
    std::is_same_v<std::remove_cvref_t<T>,
                   ReplicatedOperation::IntermediateCommit>;

template<class T>
concept InsertsDocuments = IsAnyOf<std::remove_cvref_t<T>,       //
                                   ReplicatedOperation::Insert,  //
                                   ReplicatedOperation::Update,  //
                                   ReplicatedOperation::Replace>;

template<class T>
concept UserTransaction =
    ModifiesUserTransaction<T> || FinishesUserTransactionOrIntermediate<T>;

template<class T>
concept DataDefinition = IsAnyOf<std::remove_cvref_t<T>,            //
                                 ReplicatedOperation::CreateShard,  //
                                 ReplicatedOperation::ModifyShard,  //
                                 ReplicatedOperation::DropShard,    //
                                 ReplicatedOperation::CreateIndex,  //
                                 ReplicatedOperation::DropIndex>;

using DataDefinitionOperation =
    std::variant<ReplicatedOperation::CreateShard,  //
                 ReplicatedOperation::ModifyShard,  //
                 ReplicatedOperation::DropShard,    //
                 ReplicatedOperation::CreateIndex,  //
                 ReplicatedOperation::DropIndex>;

auto operator==(ReplicatedOperation const&,
                ReplicatedOperation::OperationType const&) -> bool;
auto operator==(ReplicatedOperation::OperationType const&,
                ReplicatedOperation const&) -> bool;

auto operator<<(std::ostream&, ReplicatedOperation const&) -> std::ostream&;
auto operator<<(std::ostream&, ReplicatedOperation::OperationType const&)
    -> std::ostream&;
}  // namespace arangodb::replication2::replicated_state::document
