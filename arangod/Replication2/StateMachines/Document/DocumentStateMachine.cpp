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

#include "Replication2/StateMachines/Document/DocumentStateMachine.h"

#include "Inspection/VPack.h"
#include "Replication2/ReplicatedState/StateInterfaces.tpp"
#include "Replication2/StateMachines/Document/DocumentCore.h"
#include "Replication2/StateMachines/Document/DocumentFollowerState.h"
#include "Replication2/StateMachines/Document/DocumentLeaderState.h"
#include "Replication2/StateMachines/Document/DocumentLogEntry.h"
#include "Replication2/StateMachines/Document/DocumentStateHandlersFactory.h"
#include "Replication2/StateMachines/Document/DocumentStateShardHandler.h"
#include "Replication2/StateMachines/Document/DocumentStateSnapshotHandler.h"
#include "Transaction/Manager.h"

#include <Basics/voc-errors.h>
#include <Futures/Future.h>

using namespace arangodb::replication2::replicated_state::document;

auto DocumentCoreParameters::toSharedSlice() const -> velocypack::SharedSlice {
  VPackBuilder builder;
  velocypack::serialize(builder, *this);
  return builder.sharedSlice();
}

DocumentFactory::DocumentFactory(
    std::shared_ptr<IDocumentStateHandlersFactory> handlersFactory,
    transaction::IManager& transactionManager)
    : _handlersFactory(std::move(handlersFactory)),
      _transactionManager(transactionManager){};

auto DocumentFactory::constructFollower(
    std::unique_ptr<DocumentCore> core,
    std::shared_ptr<streams::Stream<DocumentState>> stream,
    std::shared_ptr<IScheduler> scheduler)
    -> std::shared_ptr<DocumentFollowerState> {
  return std::make_shared<DocumentFollowerState>(
      std::move(core), std::move(stream), _handlersFactory, scheduler);
}

auto DocumentFactory::constructLeader(
    std::unique_ptr<DocumentCore> core,
    std::shared_ptr<streams::ProducerStream<DocumentState>> stream)
    -> std::shared_ptr<DocumentLeaderState> {
  return std::make_shared<DocumentLeaderState>(
      std::move(core), std::move(stream), _handlersFactory,
      _transactionManager);
}

auto DocumentFactory::constructCore(TRI_vocbase_t& vocbase,
                                    GlobalLogIdentifier gid,
                                    DocumentCoreParameters coreParameters)
    -> std::unique_ptr<DocumentCore> {
  LoggerContext logContext = _handlersFactory->createLogger(gid);
  return std::make_unique<DocumentCore>(
      vocbase, std::move(gid), std::move(coreParameters), _handlersFactory,
      std::move(logContext));
}

auto DocumentFactory::constructCleanupHandler()
    -> std::shared_ptr<DocumentCleanupHandler> {
  return std::make_shared<DocumentCleanupHandler>();
}

#include "Replication2/ReplicatedState/ReplicatedStateImpl.tpp"

template struct arangodb::replication2::replicated_state::ReplicatedState<
    DocumentState>;

void DocumentCleanupHandler::drop(std::unique_ptr<DocumentCore> core) {
  core->drop();
  core.reset();
}
