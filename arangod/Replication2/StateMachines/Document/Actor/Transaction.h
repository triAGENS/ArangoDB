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

#include <type_traits>
#include <variant>

#include "Actor/Actor.h"
#include "Actor/ExitReason.h"
#include "Actor/LocalActorPID.h"
#include "Actor/LocalRuntime.h"
#include "Basics/application-exit.h"
#include "Logger/LogContextKeys.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/StateMachines/Document/DocumentFollowerState.h"
#include "Replication2/StateMachines/Document/DocumentLogEntry.h"
#include "Replication2/StateMachines/Document/ReplicatedOperation.h"

namespace arangodb::replication2::replicated_state::document::actor {

using namespace arangodb::actor;

struct TransactionState {
  explicit TransactionState(
      LoggerContext const& loggerContext,
      std::shared_ptr<IDocumentStateTransactionHandler> transactionHandler,
      std::shared_ptr<IDocumentStateErrorHandler> errorHandler,
      TransactionId trxId)
      : loggerContext(loggerContext.with<logContextKeyTrxId>(
            trxId)),  // TODO - include pid in context
        transactionHandler(std::move(transactionHandler)),
        errorHandler(std::move(errorHandler)),
        trxId(trxId) {}

  friend inline auto inspect(auto& f, TransactionState& x) {
    return f.object(x).fields(f.field("trxId", x.trxId),
                              f.field("skip", x.skip));
  }

  LoggerContext const loggerContext;
  std::shared_ptr<IDocumentStateTransactionHandler> const transactionHandler;
  std::shared_ptr<IDocumentStateErrorHandler> const errorHandler;
  TransactionId const trxId;

  // will be set to true if one of the modification operations fail (e.g.,
  // because the shard does not exist, or we have a unique constraint violation,
  // ...). In this case, we can conclude that we are replaying the log, and this
  // transaction has already been applied, so we can immediately remove it (and
  // thereby abort it), and skip all subsequent operations.
  bool skip = false;
};

namespace message {

struct ProcessEntry {
  ReplicatedOperation::UserTransactionOperation op;
  LogIndex index;

  friend inline auto inspect(auto& f, ProcessEntry& x) {
    return f.object(x).fields(f.field("op", x.op), f.field("index", x.index));
  }
};

struct TransactionMessages : std::variant<ProcessEntry> {
  using std::variant<ProcessEntry>::variant;

  friend inline auto inspect(auto& f, TransactionMessages& x) {
    return f.variant(x).unqualified().alternatives(
        arangodb::inspection::type<ProcessEntry>("processEntry"));
  }
};

}  // namespace message

template<typename Runtime>
struct TransactionHandler : HandlerBase<Runtime, TransactionState> {
  using ActorPID = typename Runtime::ActorPID;

  auto operator()(message::ProcessEntry&& msg)
      -> std::unique_ptr<TransactionState> {
    applyEntry(msg.op, msg.index);
    return std::move(this->state);
  }

  auto operator()(auto&& msg) -> std::unique_ptr<TransactionState> {
    LOG_CTX("6d904", FATAL, this->state->loggerContext)
        << "Transaction actor received unexpected message "
        << typeid(msg).name() << " " << inspection::json(msg);
    FATAL_ERROR_EXIT();
    return std::move(this->state);
  }

 private:
  template<class OpType>
  void maybeFinishActor() {
    // we also have to finish the actor in case of an intermediate
    // commit, because for the later operations in this transaction
    // we will start a separate actor
    if constexpr (FinishesUserTransaction<OpType> ||
                  std::is_same_v<OpType,
                                 ReplicatedOperation::IntermediateCommit>) {
      LOG_CTX("cddab", DEBUG, this->state->loggerContext)
          << "finishing actor " << this->self.id;
      this->finish(ExitReason::kFinished);
    }
  }

  void applyEntry(ReplicatedOperation::UserTransactionOperation const& op,
                  LogIndex index) {
    std::visit(
        [&](auto&& op) -> void {
          using OpType = std::remove_cvref_t<decltype(op)>;

          if (this->state->skip) {
            LOG_CTX("61fbb", TRACE, this->state->loggerContext)
                << "skipping entry " << op << " with index " << index
                << " on follower";
            maybeFinishActor<OpType>();
            return;
          }

          try {
            LOG_CTX("165a1", TRACE, this->state->loggerContext)
                << "applying entry " << op << " with index " << index
                << " on follower";
            auto originalRes = this->state->transactionHandler->applyEntry(op);
            auto res =
                this->state->errorHandler->handleOpResult(op, originalRes);
            if (res.fail()) {
              TRI_ASSERT(
                  this->state->errorHandler->handleOpResult(op, res).fail())
                  << res << " should have been already handled for operation "
                  << op << " during applyEntry of follower "
                  << this->state->loggerContext;
              LOG_CTX("88416", FATAL, this->state->loggerContext)
                  << "failed to apply entry " << op << " with index " << index
                  << " on follower: " << res;
              TRI_ASSERT(false) << res;
              FATAL_ERROR_EXIT();
            }
            if constexpr (ModifiesUserTransaction<OpType>) {
              if (originalRes.fail()) {
                LOG_CTX("583b4", DEBUG, this->state->loggerContext)
                    << "failed to apply entry " << op << " with index " << index
                    << " on follower: " << res
                    << " - ignoring this error and going into skip mode";
                this->state->skip = true;
                return;
              }
            }

            maybeFinishActor<OpType>();
          } catch (std::exception& e) {
            LOG_CTX("013aa", FATAL, this->state->loggerContext)
                << "caught exception while applying entry " << op << ": "
                << e.what();
            FATAL_ERROR_EXIT();
          } catch (...) {
            LOG_CTX("515fc", FATAL, this->state->loggerContext)
                << "caught unknown exception while applying entry " << op;
            FATAL_ERROR_EXIT();
          }
        },
        op);
  }
};

struct TransactionActor {
  using State = TransactionState;
  using Message = message::TransactionMessages;
  template<typename Runtime>
  using Handler = TransactionHandler<Runtime>;
  static constexpr auto typeName() -> std::string_view {
    return "TransactionActor";
  };
};

}  // namespace arangodb::replication2::replicated_state::document::actor
