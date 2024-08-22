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

#include "Replication2/ReplicatedState/StateMachines/DocumentState/DocumentStateMachineTest.h"

using namespace arangodb;
using namespace arangodb::tests;
using namespace arangodb::replication2;
using namespace arangodb::replication2::tests;
using namespace arangodb::replication2::replicated_state;
using namespace arangodb::replication2::replicated_state::document;

struct DocumentCoreTest : DocumentStateMachineTest {};

TEST_F(DocumentCoreTest, constructing_the_core_does_not_create_shard) {
  using namespace testing;

  auto factory = DocumentFactory(handlersFactoryMock, transactionManagerMock);

  // Initializing the core should have no effect on the shard handler.
  EXPECT_CALL(*shardHandlerMock, ensureShard(shardId, collectionType, _))
      .Times(0);
  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);

  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
}

TEST_F(DocumentCoreTest, dropping_the_core_with_error_messages) {
  using namespace testing;
  auto factory = DocumentFactory(handlersFactoryMock, transactionManagerMock);

  // Dropping the core should automatically drop all shards, as a result of the
  // replicated log removal.
  EXPECT_CALL(*shardHandlerMock, dropAllShards()).Times(1);
  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  ON_CALL(*shardHandlerMock, dropAllShards)
      .WillByDefault(Return(Result{TRI_ERROR_WAS_ERLAUBE}));
  core->drop();
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
}

TEST_F(DocumentStateMachineTest,
       shard_is_dropped_and_transactions_aborted_during_cleanup) {
  using namespace testing;
  auto transactionHandlerMock = handlersFactoryMock->makeRealTransactionHandler(
      &vocbaseMock, globalId, shardHandlerMock);
  ON_CALL(*handlersFactoryMock, createTransactionHandler(_, _, _))
      .WillByDefault([&](TRI_vocbase_t&, GlobalLogIdentifier const&,
                         std::shared_ptr<IDocumentStateShardHandler> const&) {
        return std::make_unique<NiceMock<MockDocumentStateTransactionHandler>>(
            transactionHandlerMock);
      });

  auto factory = DocumentFactory(handlersFactoryMock, transactionManagerMock);
  auto stream = std::make_shared<MockProducerStream>();
  auto follower = std::make_shared<DocumentFollowerStateWrapper>(
      factory.constructCore(vocbaseMock, globalId, coreParams), stream,
      handlersFactoryMock, schedulerMock);

  // Two steps are necessary before the snapshot is acquired:
  //  - all ongoing transactions are aborted
  //  - all shards are dropped
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(ReplicatedOperation::AbortAllOngoingTrx{}))
      .Times(1);
  EXPECT_CALL(*shardHandlerMock, dropAllShards()).Times(1);
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  // Resigning should abort all ongoing transactions, but not drop any shards
  // (because the shards might still be used on the next leader/follower
  // instance). Note that resigning != deleting the replicated log.
  EXPECT_CALL(*shardHandlerMock, dropAllShards()).Times(0);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(ReplicatedOperation::AbortAllOngoingTrx{}))
      .Times(1);
  auto core = std::move(*follower).resign();
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  // Dropping the core should drop all the shards, but no longer explicitly
  // abort any transactions (because it is not needed, since the follower
  // resigned already).
  auto cleanupHandler = factory.constructCleanupHandler();
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(ReplicatedOperation::AbortAllOngoingTrx{}))
      .Times(0);
  EXPECT_CALL(*shardHandlerMock, dropAllShards()).Times(1);
  cleanupHandler->drop(std::move(core));
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
}
