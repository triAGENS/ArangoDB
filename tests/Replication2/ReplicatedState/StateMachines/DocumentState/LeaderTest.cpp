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

#include "Mocks/Death_Test.h"
#include "Replication2/ReplicatedState/StateMachines/DocumentState/DocumentStateMachineTest.h"
#include "Transaction/StandaloneContext.h"

using namespace arangodb;
using namespace arangodb::tests;
using namespace arangodb::replication2;
using namespace arangodb::replication2::tests;
using namespace arangodb::replication2::replicated_state;
using namespace arangodb::replication2::replicated_state::document;

struct DocumentStateLeaderTest : DocumentStateMachineTest {};
struct DocumentStateLeaderDeathTest : DocumentStateLeaderTest {};

TEST_F(DocumentStateLeaderTest, leader_manipulates_snapshot_successfully) {
  using namespace testing;

  cluster::RebootTracker fakeRebootTracker{nullptr};
  fakeRebootTracker.updateServerState(
      {{"documentStateMachineServer",
        ServerHealthState{RebootId{1}, arangodb::ServerHealth::kUnclear}}});

  auto snapshotHandler =
      handlersFactoryMock->makeRealSnapshotHandler(&fakeRebootTracker);
  ON_CALL(*handlersFactoryMock, createSnapshotHandler(_, _))
      .WillByDefault([&](TRI_vocbase_t&, GlobalLogIdentifier const& gid) {
        return snapshotHandler;
      });

  auto leader = createLeader();
  EXPECT_CALL(*snapshotHandler, create(_, _)).Times(1);
  auto snapshotStartRes = leader->snapshotStart(SnapshotParams::Start{
      .serverId = "documentStateMachineServer", .rebootId = RebootId{1}});
  EXPECT_TRUE(snapshotStartRes.ok()) << snapshotStartRes.result();
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  auto snapshotId = snapshotStartRes.get().snapshotId;

  EXPECT_CALL(*snapshotHandler, find(snapshotId)).Times(1);
  auto snapshotNextRes = leader->snapshotNext(SnapshotParams::Next{snapshotId});
  EXPECT_TRUE(snapshotNextRes.ok()) << snapshotNextRes.result();
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  EXPECT_CALL(*snapshotHandler, finish(snapshotId)).Times(1);
  auto snapshotFinishRes =
      leader->snapshotFinish(SnapshotParams::Finish{snapshotId});
  EXPECT_TRUE(snapshotFinishRes.ok()) << snapshotFinishRes;
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  // The snapshot should be cleared after finish was called
  EXPECT_CALL(*snapshotHandler, find(snapshotId)).Times(1);
  auto snapshotStatusRes = leader->snapshotStatus(snapshotId);
  EXPECT_TRUE(snapshotStatusRes.fail());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  EXPECT_TRUE(leader->allSnapshotsStatus().ok());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
}

TEST_F(DocumentStateLeaderTest, leader_manipulates_snapshots_with_errors) {
  using namespace testing;

  auto snapshotHandler = handlersFactoryMock->makeRealSnapshotHandler();
  ON_CALL(*handlersFactoryMock, createSnapshotHandler(_, _))
      .WillByDefault([&](TRI_vocbase_t&, GlobalLogIdentifier const& gid) {
        return snapshotHandler;
      });
  ON_CALL(*snapshotHandler, create(_, _))
      .WillByDefault(Return(
          ResultT<std::weak_ptr<Snapshot>>::error(TRI_ERROR_WAS_ERLAUBE)));
  ON_CALL(*snapshotHandler, find(SnapshotId{1}))
      .WillByDefault(Return(
          ResultT<std::weak_ptr<Snapshot>>::error(TRI_ERROR_WAS_ERLAUBE)));

  auto leader = createLeader();
  EXPECT_TRUE(leader->snapshotStart(SnapshotParams::Start{}).fail());
  EXPECT_TRUE(leader->snapshotNext(SnapshotParams::Next{SnapshotId{1}}).fail());
  EXPECT_TRUE(
      leader->snapshotFinish(SnapshotParams::Finish{SnapshotId{1}}).fail());
  EXPECT_TRUE(leader->snapshotStatus(SnapshotId{1}).fail());
}

TEST_F(DocumentStateLeaderTest,
       leader_resign_should_abort_active_transactions) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<testing::NiceMock<MockProducerStream>>();
  auto leaderState = factory.constructLeader(std::move(core), stream);

  {
    for (std::uint64_t tid : {5, 9, 13}) {
      auto res = leaderState
                     ->replicateOperation(
                         ReplicatedOperation::buildDocumentOperation(
                             TRI_VOC_DOCUMENT_OPERATION_INSERT,
                             TransactionId{tid}.asFollowerTransactionId(),
                             shardId, velocypack::SharedSlice(), "root"),
                         ReplicationOptions{})
                     .waitAndGet();
      EXPECT_TRUE(res.ok()) << res.result();
    }
  }
  EXPECT_EQ(3U, leaderState->getActiveTransactionsCount());

  {
    VPackBuilder builder;
    auto res = leaderState
                   ->replicateOperation(
                       ReplicatedOperation::buildAbortOperation(
                           TransactionId{5}.asFollowerTransactionId()),
                       ReplicationOptions{})
                   .waitAndGet();
    EXPECT_TRUE(res.ok()) << res.result();
    leaderState->release(TransactionId{5}.asFollowerTransactionId(), res.get());

    res = leaderState
              ->replicateOperation(
                  ReplicatedOperation::buildCommitOperation(
                      TransactionId{9}.asFollowerTransactionId()),
                  ReplicationOptions{})
              .waitAndGet();
    EXPECT_TRUE(res.ok()) << res.result();
    leaderState->release(TransactionId{9}.asFollowerTransactionId(), res.get());
  }
  EXPECT_EQ(1U, leaderState->getActiveTransactionsCount());

  // resigning should abort the remaining transaction with ID 13
  EXPECT_CALL(transactionManagerMock,
              abortManagedTrx(TransactionId{13}, globalId.database))
      .Times(1);

  // resigning should abort all ongoing transactions
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(ReplicatedOperation::AbortAllOngoingTrx{}))
      .Times(1);

  std::ignore = std::move(*leaderState).resign();
  Mock::VerifyAndClearExpectations(&transactionManagerMock);
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
}

TEST_F(DocumentStateLeaderTest,
       recoverEntries_should_abort_remaining_active_transactions) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(ReplicatedOperation::buildCreateShardOperation(
      shardId, TRI_COL_TYPE_DOCUMENT, velocypack::SharedSlice()));
  // Transaction IDs are of follower type, as if they were replicated.
  entries.emplace_back(createDocumentEntry(TransactionId{6}));
  entries.emplace_back(createDocumentEntry(TransactionId{10}));
  entries.emplace_back(createDocumentEntry(TransactionId{14}));
  entries.emplace_back(
      ReplicatedOperation::buildAbortOperation(TransactionId{6}));
  entries.emplace_back(
      ReplicatedOperation::buildCommitOperation(TransactionId{10}));

  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<MockProducerStream>();
  EXPECT_CALL(*stream, getCommittedMetadata).Times(1);
  auto leaderState = factory.constructLeader(std::move(core), stream);
  testing::Mock::VerifyAndClearExpectations(stream.get());

  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  EXPECT_CALL(*stream, insert)
      .WillOnce([&](auto const& entry, bool waitForSync) {
        EXPECT_EQ(entry.operation,
                  ReplicatedOperation::buildAbortAllOngoingTrxOperation());
        EXPECT_FALSE(waitForSync);
        return LogIndex{entries.size() + 1};
      });
  EXPECT_CALL(transactionManagerMock,
              abortManagedTrx(TransactionId{14}.asLeaderTransactionId(),
                              globalId.database))
      .Times(1);
  EXPECT_CALL(*transactionMock, apply).Times(3);
  EXPECT_CALL(*transactionMock, commit).Times(1);
  EXPECT_CALL(*transactionMock, abort).Times(1);

  std::ignore = leaderState->recoverEntries(std::move(entryIterator));

  Mock::VerifyAndClearExpectations(&transactionManagerMock);
  Mock::VerifyAndClearExpectations(transactionMock.get());
}

TEST_F(DocumentStateLeaderTest,
       recoverEntries_should_abort_transactions_before_dropping_shard) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  ON_CALL(*transactionHandlerMock, getTransactionsForShard(shardId))
      .WillByDefault(Return(std::vector<TransactionId>{
          TransactionId{6}, TransactionId{10}, TransactionId{14}}));

  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(createDocumentEntry(TransactionId{6}));
  entries.emplace_back(createDocumentEntry(TransactionId{10}));
  entries.emplace_back(createDocumentEntry(TransactionId{14}));
  entries.emplace_back(ReplicatedOperation::buildDropShardOperation(shardId));

  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<MockProducerStream>();
  EXPECT_CALL(*stream, getCommittedMetadata).Times(1);
  auto leaderState = factory.constructLeader(std::move(core), stream);
  Mock::VerifyAndClearExpectations(stream.get());

  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  EXPECT_CALL(*stream, insert).Times(1);
  EXPECT_CALL(*transactionMock, abort).Times(3);
  std::ignore = leaderState->recoverEntries(std::move(entryIterator));
  Mock::VerifyAndClearExpectations(transactionMock.get());
}

TEST_F(DocumentStateLeaderDeathTest,
       leader_recoverEntries_dies_if_vocbase_does_not_exist) {
  using namespace testing;

  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(createDocumentEntry(TransactionId{10}));

  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<MockProducerStream>();
  EXPECT_CALL(*stream, getCommittedMetadata).Times(1);
  auto leaderState = factory.constructLeader(std::move(core), stream);
  testing::Mock::VerifyAndClearExpectations(stream.get());

  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  ASSERT_DEATH_CORE_FREE(
      std::ignore = leaderState->recoverEntries(std::move(entryIterator)), "");
}

TEST_F(DocumentStateLeaderTest,
       leader_should_not_replicate_unknown_transactions) {
  using namespace testing;

  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<MockProducerStream>();
  EXPECT_CALL(*stream, getCommittedMetadata).Times(1);
  auto leaderState = factory.constructLeader(std::move(core), stream);
  testing::Mock::VerifyAndClearExpectations(stream.get());

  ReplicatedOperation operation{
      ReplicatedOperation::Commit{TransactionId{5}.asFollowerTransactionId()}};
  EXPECT_FALSE(leaderState->needsReplication(operation));

  operation.operation.emplace<ReplicatedOperation::Insert>(
      TransactionId{5}.asFollowerTransactionId(), shardId,
      velocypack::SharedSlice(), "root");
  EXPECT_TRUE(leaderState->needsReplication(operation));

  operation.operation.emplace<ReplicatedOperation::Commit>(
      TransactionId{5}.asLeaderTransactionId());
  EXPECT_FALSE(leaderState->needsReplication(operation));
}

TEST_F(DocumentStateLeaderTest,
       leader_ignores_invalid_transactions_during_recovery) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<MockProducerStream>();
  EXPECT_CALL(*stream, getCommittedMetadata).Times(1);
  auto leaderState = factory.constructLeader(std::move(core), stream);
  testing::Mock::VerifyAndClearExpectations(stream.get());

  // Try to apply a regular entry, not having the shard available
  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(createDocumentEntry(TransactionId{6}));
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*stream, insert).Times(1);  // AbortAllOngoingTrx
  EXPECT_CALL(*stream, release).Times(1);
  ON_CALL(*transactionHandlerMock,
          applyEntry(std::get<ReplicatedOperation::Insert>(
              entries[0].getInnerOperation())))
      .WillByDefault(Return(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  std::ignore = leaderState->recoverEntries(std::move(entryIterator));
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
  Mock::VerifyAndClearExpectations(transactionMock.get());
  Mock::VerifyAndClearExpectations(stream.get());

  // Try to commit the previous entry, but nothing should get committed
  entries.clear();
  entries.emplace_back(
      ReplicatedOperation{ReplicatedOperation::Commit(TransactionId{6})});
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*stream, insert).Times(1);  // AbortAllOngoingTrx
  EXPECT_CALL(*stream, release).Times(1);
  EXPECT_CALL(*transactionMock, commit()).Times(0);
  std::ignore = leaderState->recoverEntries(std::move(entryIterator));
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
  Mock::VerifyAndClearExpectations(transactionMock.get());
  Mock::VerifyAndClearExpectations(stream.get());
}

TEST_F(DocumentStateLeaderTest, leader_create_modify_and_drop_shard) {
  using namespace testing;

  DocumentFactory factory =
      DocumentFactory(handlersFactoryMock, transactionManagerMock);

  auto core = factory.constructCore(vocbaseMock, globalId, coreParams);
  auto stream = std::make_shared<testing::NiceMock<MockProducerStream>>();
  auto leaderState = factory.constructLeader(std::move(core), stream);

  auto properties = velocypack::SharedSlice();

  // CreateShard
  EXPECT_CALL(*stream, insert)
      .Times(1)
      .WillOnce([&](DocumentLogEntry const& entry, bool waitForSync) {
        EXPECT_EQ(entry.operation,
                  ReplicatedOperation::CreateShard(
                      shardId, TRI_COL_TYPE_DOCUMENT, properties));
        EXPECT_TRUE(waitForSync);
        return LogIndex{12};
      });

  EXPECT_CALL(*stream, waitFor(LogIndex{12})).Times(1).WillOnce([](auto) {
    return futures::Future<MockProducerStream::WaitForResult>{
        MockProducerStream::WaitForResult{}};
  });

  EXPECT_CALL(*stream, release(LogIndex{12})).Times(1);

  EXPECT_CALL(*shardHandlerMock, ensureShard(shardId, TRI_COL_TYPE_DOCUMENT, _))
      .Times(1);

  auto res =
      leaderState->createShard(shardId, TRI_COL_TYPE_DOCUMENT, properties)
          .waitAndGet();
  EXPECT_TRUE(res.ok()) << res;

  Mock::VerifyAndClearExpectations(stream.get());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  // ModifyShard
  auto context = std::make_shared<transaction::StandaloneContext>(
      vocbaseMock, transaction::OperationOriginTestCase{});
  auto methods = std::make_unique<transaction::Methods>(context);
  EXPECT_CALL(*shardHandlerMock, lockShard(_, _, _))
      .Times(1)
      .WillOnce(Return(ResultT<std::unique_ptr<transaction::Methods>>::success(
          std::move(methods))));
  std::string followersToDrop{"hund,katze"};
  EXPECT_CALL(*stream, insert)
      .Times(1)
      .WillOnce([&](DocumentLogEntry const& entry, bool waitForSync) {
        EXPECT_EQ(entry.operation,
                  ReplicatedOperation::ModifyShard(shardId, collectionId,
                                                   velocypack::SharedSlice()));
        EXPECT_TRUE(waitForSync);
        return LogIndex{12};
      });

  EXPECT_CALL(*stream, waitFor(LogIndex{12})).Times(1).WillOnce([](auto) {
    return futures::Future<MockProducerStream::WaitForResult>{
        MockProducerStream::WaitForResult{}};
  });

  EXPECT_CALL(*stream, release(LogIndex{12})).Times(1);

  EXPECT_CALL(*shardHandlerMock, modifyShard(shardId, collectionId, _))
      .Times(1);

  res =
      leaderState->modifyShard(shardId, collectionId, velocypack::SharedSlice())
          .waitAndGet();
  EXPECT_TRUE(res.ok()) << res;

  Mock::VerifyAndClearExpectations(stream.get());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());

  // DropShard
  EXPECT_CALL(*stream, insert)
      .Times(1)
      .WillOnce([&](DocumentLogEntry const& entry, bool waitForSync) {
        EXPECT_EQ(entry.operation,
                  ReplicatedOperation{ReplicatedOperation::DropShard(shardId)});
        EXPECT_TRUE(waitForSync);
        return LogIndex{12};
      });

  EXPECT_CALL(*stream, waitFor(LogIndex{12})).Times(1).WillOnce([](auto) {
    return futures::Future<MockProducerStream::WaitForResult>{
        MockProducerStream::WaitForResult{}};
  });

  EXPECT_CALL(*stream, release(LogIndex{12})).Times(1);

  EXPECT_CALL(*shardHandlerMock, dropShard(shardId)).Times(1);

  std::ignore = leaderState->dropShard(shardId);
}
