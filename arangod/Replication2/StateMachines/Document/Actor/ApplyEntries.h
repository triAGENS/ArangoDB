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
/// @author Manuel Pöter
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <optional>

#include "Actor/Actor.h"
#include "Actor/ExitReason.h"
#include "Actor/LocalActorPID.h"
#include "Basics/application-exit.h"
#include "Inspection/Transformers.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/StateMachines/Document/ActiveTransactionsQueue.h"
#include "Replication2/StateMachines/Document/DocumentFollowerState.h"
#include "Replication2/StateMachines/Document/DocumentLogEntry.h"
#include "VocBase/Identifiers/TransactionId.h"

namespace arangodb::replication2::replicated_state::document::actor {

using namespace arangodb::actor;

struct ApplyEntriesState {
  explicit ApplyEntriesState(LoggerContext const& loggerContext,
                             DocumentFollowerState& followerState)
      : loggerContext(loggerContext),  // TODO - include pid in context
        followerState(followerState) {}

  friend inline auto inspect(auto& f, ApplyEntriesState& x) {
    return f.object(x).fields(
        f.field("transactionMap", x._transactionMap)
            .transformWith(inspection::mapToListTransformer(x._transactionMap)),
        f.field("pendingTransactions", x._pendingTransactions)
            .transformWith(
                inspection::mapToListTransformer(x._pendingTransactions)),
        f.field("batch", x._batch));
  }

  struct Batch {
    Batch(std::unique_ptr<DocumentFollowerState::EntryIterator>&& entries,
          futures::Promise<ResultT<std::optional<LogIndex>>>&& promise)
        : entries(std::move(entries)),
          promise(std::move(promise)),
          _currentEntry(this->entries->next()) {}

    std::unique_ptr<DocumentFollowerState::EntryIterator> entries;
    futures::Promise<ResultT<std::optional<LogIndex>>> promise;

    std::optional<std::pair<LogIndex, DocumentLogEntry>> _currentEntry;
    std::optional<LogIndex> lastIndex;

    friend inline auto inspect(auto& f, Batch& x) {
      return f.object(x).fields(f.field("currentEntry", x._currentEntry),
                                f.field("lastIndex", x.lastIndex));
    }
  };

  LoggerContext const loggerContext;
  DocumentFollowerState& followerState;

  std::unique_ptr<Batch> _batch;

  // map of currently ongoing transactions to their respective actor pids
  std::unordered_map<TransactionId, LocalActorPID> _transactionMap;

  ActiveTransactionsQueue activeTransactions;

  struct TransactionInfo {
    TransactionId tid;
    bool intermediateCommit;

    friend inline auto inspect(auto& f, TransactionInfo& x) {
      return f.object(x).fields(
          f.field("tid", x.tid),
          f.field("intermediateCommit", x.intermediateCommit));
    }
  };
  // list of pending transactions - these are transactions which have been
  // sent a commit message, but have not yet finished
  // We keep the transaction id and the information whether this actor was
  // finished with in intermediate commit or not, because for completed
  // transaction this actor is responsible to remove the transaction from the
  // transaction handler
  std::unordered_map<LocalActorPID, TransactionInfo> _pendingTransactions;
};

namespace message {

struct ApplyEntries {
  std::unique_ptr<DocumentFollowerState::EntryIterator> entries;
  futures::Promise<ResultT<std::optional<LogIndex>>> promise;

  friend inline auto inspect(auto& f, ApplyEntries& x) {
    return f.object(x).fields();
  }
};

struct Resign {
  friend inline auto inspect(auto& f, Resign& x) {
    return f.object(x).fields();
  }
};

struct ApplyEntriesMessages : std::variant<ApplyEntries, Resign> {
  using std::variant<ApplyEntries, Resign>::variant;

  friend inline auto inspect(auto& f, ApplyEntriesMessages& x) {
    return f.variant(x).unqualified().alternatives(
        arangodb::inspection::type<ApplyEntries>("applyEntries"),
        arangodb::inspection::type<Resign>("resign"));
  }
};

}  // namespace message

template<typename Runtime>
struct ApplyEntriesHandler : HandlerBase<Runtime, ApplyEntriesState> {
  using ActorPID = typename Runtime::ActorPID;

  auto operator()(message::ApplyEntries&& msg) noexcept
      -> std::unique_ptr<ApplyEntriesState>;

  auto operator()(message::Resign&& msg) noexcept
      -> std::unique_ptr<ApplyEntriesState>;

  auto operator()(
      arangodb::actor::message::ActorDown<typename Runtime::ActorPID>&&
          msg) noexcept -> std::unique_ptr<ApplyEntriesState>;

  auto operator()(auto&& msg) noexcept -> std::unique_ptr<ApplyEntriesState> {
    LOG_CTX("0bc2e", FATAL, this->state->loggerContext)
        << "ApplyEntries actor received unexpected message "
        << typeid(msg).name() << " " << inspection::json(msg);
    FATAL_ERROR_EXIT();
    return std::move(this->state);
  }

 private:
  enum class ProcessResult {
    kContinue,
    kWaitForPendingTrx,  // indicates that the current entry cannot be processed
                         // until all pending transactions have finished
    kMoveToNextEntryAndWaitForPendingTrx  // indicates that the current entry
                                          // has been processed, but we need to
                                          // wait for all pending transactions
                                          // to finish before we we process the
                                          // next entry
  };

  void continueBatch() noexcept;

  void resign();

  void resolveBatch(Result result);

  auto processEntry(DataDefinition auto& op, LogIndex)
      -> ResultT<ProcessResult>;
  auto processEntry(ReplicatedOperation::AbortAllOngoingTrx& op, LogIndex index)
      -> ResultT<ProcessResult>;
  auto processEntry(UserTransaction auto& op, LogIndex index)
      -> ResultT<ProcessResult>;

  auto beforeApplyEntry(ModifiesUserTransaction auto const& op, LogIndex index)
      -> bool;
  auto beforeApplyEntry(ReplicatedOperation::IntermediateCommit const& op,
                        LogIndex) -> bool;
  auto beforeApplyEntry(FinishesUserTransaction auto const& op, LogIndex index)
      -> bool;

  auto applyDataDefinitionEntry(ReplicatedOperation::DropShard const& op,
                                LogIndex index) -> Result;

  auto applyDataDefinitionEntry(ReplicatedOperation::ModifyShard const& op,
                                LogIndex index) -> Result;
  template<class T>
  requires IsAnyOf<T, ReplicatedOperation::CreateShard,
                   ReplicatedOperation::CreateIndex,
                   ReplicatedOperation::DropIndex>
  auto applyDataDefinitionEntry(T const& op, LogIndex index) -> Result;

  template<class T>
  auto applyEntryAndReleaseIndex(T const& op, LogIndex index) -> Result;
};

struct ApplyEntriesActor {
  using State = ApplyEntriesState;
  using Message = message::ApplyEntriesMessages;
  template<typename Runtime>
  using Handler = ApplyEntriesHandler<Runtime>;
  static constexpr auto typeName() -> std::string_view {
    return "ApplyEntriesActor";
  };
};

}  // namespace arangodb::replication2::replicated_state::document::actor
