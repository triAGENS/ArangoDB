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

#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/StateMachines/Document/DocumentStateErrorHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateMachine.h"
#include "Replication2/StateMachines/Document/ReplicatedOperation.h"
#include "Replication2/StateMachines/Document/LowestSafeIndexesForReplay.h"
#include "Replication2/Streams/Streams.h"

#include "Basics/Guarded.h"
#include "Transaction/Options.h"
#include "VocBase/Identifiers/TransactionId.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace arangodb::replication2::replicated_state::document {

struct IDocumentStateTransaction;
struct IDocumentStateHandlersFactory;
struct IDocumentStateShardHandler;
class DocumentStateTransaction;

struct IDocumentStateTransactionHandler {
  using TransactionMap =
      std::unordered_map<TransactionId,
                         std::shared_ptr<IDocumentStateTransaction>>;

  virtual ~IDocumentStateTransactionHandler() = default;

  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Commit const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Abort const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::IntermediateCommit const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Truncate const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Insert const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Update const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Replace const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::Remove const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::AbortAllOngoingTrx const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::CreateShard const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::ModifyShard const&) noexcept -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::DropShard const&) noexcept -> Result = 0;
  // TODO These should return futures, and maybe some others, too
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::CreateIndex const&, LogIndex,
      LowestSafeIndexesForReplay&, streams::Stream<DocumentState>&) noexcept
      -> Result = 0;
  [[nodiscard]] virtual auto applyEntry(
      ReplicatedOperation::DropIndex const&) noexcept -> Result = 0;

  virtual void removeTransaction(TransactionId tid) = 0;

  virtual auto getTransactionsForShard(ShardID const&)
      -> std::vector<TransactionId> = 0;
  [[nodiscard]] virtual auto getUnfinishedTransactions() const
      -> TransactionMap = 0;
};

class DocumentStateTransactionHandler
    : public IDocumentStateTransactionHandler {
 public:
  explicit DocumentStateTransactionHandler(
      GlobalLogIdentifier gid, TRI_vocbase_t* vocbase,
      std::shared_ptr<IDocumentStateHandlersFactory> factory,
      std::shared_ptr<IDocumentStateShardHandler> shardHandler);
  auto applyEntry(ReplicatedOperation::Commit const& commit) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::Abort const& abort1) noexcept
      -> Result override;
  auto applyEntry(
      const ReplicatedOperation::IntermediateCommit& commit) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::Truncate const& truncate) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::Insert const& insert) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::Update const& update) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::Replace const& replace) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::Remove const& remove1) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::AbortAllOngoingTrx const& trx) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::CreateShard const& shard) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::ModifyShard const& shard) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::DropShard const& shard) noexcept
      -> Result override;
  auto applyEntry(ReplicatedOperation::CreateIndex const& index, LogIndex,
                  LowestSafeIndexesForReplay&,
                  streams::Stream<DocumentState>&) noexcept -> Result override;
  auto applyEntry(ReplicatedOperation::DropIndex const& index) noexcept
      -> Result override;

  // inherit convenience methods
  using IDocumentStateTransactionHandler::applyEntry;

  void removeTransaction(TransactionId tid) override;

  auto getTransactionsForShard(ShardID const&)
      -> std::vector<TransactionId> override;

  [[nodiscard]] auto getUnfinishedTransactions() const
      -> TransactionMap override;

 private:
  auto getTrx(TransactionId tid) -> std::shared_ptr<IDocumentStateTransaction>;
  void setTrx(TransactionId tid,
              std::shared_ptr<IDocumentStateTransaction> trx);

  auto applyOp(FinishesUserTransaction auto const&) -> Result;
  auto applyOp(ReplicatedOperation::IntermediateCommit const&) -> Result;
  auto applyOp(ModifiesUserTransaction auto const&) -> Result;
  auto applyOp(ReplicatedOperation::AbortAllOngoingTrx const&) -> Result;
  auto applyOp(ReplicatedOperation::CreateShard const&) -> Result;
  auto applyOp(ReplicatedOperation::ModifyShard const&) -> Result;
  auto applyOp(ReplicatedOperation::DropShard const&) -> Result;

  // TODO These should return futures
  auto applyOp(ReplicatedOperation::CreateIndex const&, LogIndex index,
               LowestSafeIndexesForReplay& lowestSafeIndexesForReplay,
               streams::Stream<DocumentState>& stream) -> Result;
  auto applyOp(ReplicatedOperation::DropIndex const&) -> Result;

  template<typename Op, typename... Args>
  auto applyAndCatchAndLog(Op&& op, Args&&... args) -> Result;

 private:
  GlobalLogIdentifier const _gid;
  TRI_vocbase_t* const _vocbase;
  LoggerContext const _loggerContext;
  std::shared_ptr<IDocumentStateHandlersFactory> const _factory;
  std::shared_ptr<IDocumentStateShardHandler> const _shardHandler;
  std::shared_ptr<IDocumentStateErrorHandler> const _errorHandler;
  Guarded<TransactionMap> _transactions;
};

}  // namespace arangodb::replication2::replicated_state::document
