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

using namespace arangodb;
using namespace arangodb::tests;
using namespace arangodb::replication2;
using namespace arangodb::replication2::tests;
using namespace arangodb::replication2::replicated_state;
using namespace arangodb::replication2::replicated_state::document;

struct DocumentStateFollowerTest : DocumentStateMachineTest {};
struct DocumentStateFollowerDeathTest : DocumentStateFollowerTest {};

TEST_F(DocumentStateFollowerTest, follower_associated_shard_map) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());

  auto shards = logicalCollections;
  auto shard = makeLogicalCollection(shardId);
  shards.emplace_back(std::move(shard));

  EXPECT_CALL(*shardHandlerMock, getAvailableShards())
      .Times(1)
      .WillOnce(Return(shards));

  auto shardIds = follower->getAssociatedShardList();
  EXPECT_EQ(shardIds.size(), 1);
  EXPECT_EQ(shardIds[0], shardId);
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
}

TEST_F(DocumentStateFollowerTest,
       follower_acquire_snapshot_calls_leader_interface) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();

  // The first call to applyEntry should be AbortAllOngoingTrx
  // Then we intentionally insert two more entries (which are also
  // AbortAllOngoingTrx, for simplicity)
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::AbortAllOngoingTrx const&>()))
      .Times(3);
  EXPECT_CALL(*leaderInterfaceMock, startSnapshot())
      .Times(1)
      .WillOnce(Return(futures::Future<ResultT<SnapshotBatch>>{
          std::in_place,
          SnapshotBatch{
              .snapshotId = SnapshotId{1},
              .hasMore = true,
              .operations = {
                  ReplicatedOperation::buildAbortAllOngoingTrxOperation()}}}));
  EXPECT_CALL(*leaderInterfaceMock, nextSnapshotBatch(SnapshotId{1}))
      .Times(1)
      .WillOnce(Return(futures::Future<ResultT<SnapshotBatch>>{
          std::in_place,
          SnapshotBatch{
              .snapshotId = SnapshotId{1},
              .hasMore = false,
              .operations = {
                  ReplicatedOperation::buildAbortAllOngoingTrxOperation()}}}));
  EXPECT_CALL(*leaderInterfaceMock, finishSnapshot(SnapshotId{1})).Times(1);
  EXPECT_CALL(*networkHandlerMock, getLeaderInterface("participantId"))
      .Times(1);

  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());

  Mock::VerifyAndClearExpectations(networkHandlerMock.get());
  Mock::VerifyAndClearExpectations(leaderInterfaceMock.get());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
}

TEST_F(DocumentStateFollowerTest,
       follower_resigning_while_acquiring_snapshot_concurrently) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();

  MockDocumentStateSnapshotHandler::rebootTracker.updateServerState(
      {{"participantId",
        ServerHealthState{RebootId{1}, arangodb::ServerHealth::kUnclear}}});

  std::atomic<bool> acquireSnapshotCalled = false;
  std::atomic<bool> followerResigned = false;
  int batchesSent = 0;

  // The snapshot will not stop until the follower resigns
  ON_CALL(*leaderInterfaceMock, startSnapshot).WillByDefault([&]() {
    acquireSnapshotCalled.store(true);
    acquireSnapshotCalled.notify_one();
    return futures::Future<ResultT<SnapshotBatch>>{
        std::in_place,
        SnapshotBatch{
            .snapshotId = SnapshotId{1}, .hasMore = true, .operations = {}}};
  });
  ON_CALL(*leaderInterfaceMock, nextSnapshotBatch)
      .WillByDefault([&](SnapshotId id) {
        // In the event that the system is under heavy load, we want to prevent
        // a stack overflow
        ++batchesSent;
        if (batchesSent > 16) {
          followerResigned.wait(false);
        }

        return futures::Future<ResultT<SnapshotBatch>>{
            std::in_place,
            SnapshotBatch{.snapshotId = id, .hasMore = true, .operations = {}}};
      });

  std::thread t([follower]() {
    auto res = follower->acquireSnapshot("participantId");
    EXPECT_TRUE(res.isReady());
    EXPECT_TRUE(res.waitAndGet().fail());
    EXPECT_TRUE(res.waitAndGet().errorNumber() ==
                TRI_ERROR_REPLICATION_REPLICATED_LOG_FOLLOWER_RESIGNED);
  });

  // Wait for the snapshot to start
  acquireSnapshotCalled.wait(false);

  std::move(*follower).resign();

  // Let the other thread know that the follower resigned
  followerResigned.store(true);
  followerResigned.notify_one();

  t.join();
}

TEST_F(DocumentStateFollowerTest,
       follower_applyEntries_encounters_AbortAllOngoingTrx_and_aborts_all_trx) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  std::vector<DocumentLogEntry> entries;
  for (std::uint64_t tid : {6, 10, 14}) {
    entries.emplace_back(createDocumentEntry(TransactionId{tid}));
  }
  entries.emplace_back(ReplicatedOperation::buildAbortAllOngoingTrxOperation());

  // AbortAllOngoingTrx should count towards the release index
  auto expectedReleaseIndex = LogIndex{4};
  for (std::uint64_t tid : {18, 22}) {
    entries.emplace_back(createDocumentEntry(TransactionId{tid}));
  }

  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  EXPECT_CALL(*stream, release).WillOnce([&](LogIndex index) {
    EXPECT_EQ(index, expectedReleaseIndex);
  });
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
}

TEST_F(DocumentStateFollowerTest,
       follower_applyEntries_applies_transactions_but_does_not_release) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  std::vector<DocumentLogEntry> entries;
  for (std::uint64_t tid : {6, 10, 14}) {
    entries.emplace_back(createDocumentEntry(TransactionId{tid}));
  }

  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  // We only call release on commit or abort
  EXPECT_CALL(*stream, release).Times(0);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Insert const&>()))
      .Times(3);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
}

TEST_F(DocumentStateFollowerTest,
       follower_intermediate_commit_does_not_release) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  std::vector<DocumentLogEntry> entries;
  auto tid = TransactionId{6};
  entries.emplace_back(createDocumentEntry(TransactionId{tid}));
  entries.emplace_back(ReplicatedOperation::buildIntermediateCommitOperation(
      TransactionId{tid}));
  entries.emplace_back(
      ReplicatedOperation::buildIntermediateCommitOperation(TransactionId{8}));

  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  EXPECT_CALL(*stream, release).Times(0);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(stream.get());
}

TEST_F(DocumentStateFollowerDeathTest,
       follower_applyEntries_dies_if_transaction_fails) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  ON_CALL(*transactionHandlerMock,
          applyEntry(An<ReplicatedOperation::Insert const&>()))
      .WillByDefault(Return(Result(TRI_ERROR_WAS_ERLAUBE)));
  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(ReplicatedOperation{ReplicatedOperation::Insert{
      TransactionId{6}, shardId, velocypack::SharedSlice(), "root"}});
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  ASSERT_DEATH_CORE_FREE(
      std::ignore =
          follower->applyEntries(std::move(entryIterator)).waitAndGet(),
      "");
}

TEST_F(DocumentStateFollowerTest,
       follower_applyEntries_commit_and_abort_call_release) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  // First commit then abort
  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(createDocumentEntry(TransactionId{6}));
  entries.emplace_back(createDocumentEntry(TransactionId{10}));
  entries.emplace_back(
      ReplicatedOperation::buildCommitOperation(TransactionId{6}));
  entries.emplace_back(createDocumentEntry(TransactionId{14}));
  entries.emplace_back(createDocumentEntry(TransactionId{18}));
  entries.emplace_back(
      ReplicatedOperation::buildAbortOperation(TransactionId{10}));
  entries.emplace_back(createDocumentEntry(TransactionId{22}));
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*stream, release).WillOnce([&](LogIndex index) {
    EXPECT_EQ(index.value, 3);
  });
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Insert const&>()))
      .Times(5);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Commit const&>()))
      .Times(1);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Abort const&>()))
      .Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(stream.get());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());

  // First abort then commit
  follower = createFollower();
  res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  stream = follower->stream;
  entries.clear();

  entries.emplace_back(createDocumentEntry(TransactionId{6}));
  entries.emplace_back(createDocumentEntry(TransactionId{10}));
  entries.emplace_back(
      ReplicatedOperation::buildAbortOperation(TransactionId{6}));
  entries.emplace_back(createDocumentEntry(TransactionId{14}));
  entries.emplace_back(createDocumentEntry(TransactionId{18}));
  entries.emplace_back(
      ReplicatedOperation::buildCommitOperation(TransactionId{10}));
  entries.emplace_back(createDocumentEntry(TransactionId{22}));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*stream, release).WillOnce([&](LogIndex index) {
    EXPECT_EQ(index.value, 3);
  });
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Insert const&>()))
      .Times(5);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Commit const&>()))
      .Times(1);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(An<ReplicatedOperation::Abort const&>()))
      .Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
}

TEST_F(DocumentStateFollowerTest,
       follower_applyEntries_creates_modifies_and_drops_shard) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());

  auto stream = follower->stream;

  ShardID const myShard{12};
  CollectionID const myCollection = "myCollection";

  // CreateShard
  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(ReplicatedOperation::buildCreateShardOperation(
      myShard, TRI_COL_TYPE_DOCUMENT, velocypack::SharedSlice()));
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*shardHandlerMock, ensureShard(myShard, TRI_COL_TYPE_DOCUMENT, _))
      .Times(1);
  EXPECT_CALL(*stream, release).Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(stream.get());

  // ModifyShard
  entries.clear();
  entries.emplace_back(ReplicatedOperation::buildModifyShardOperation(
      myShard, myCollection, velocypack::SharedSlice()));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*shardHandlerMock, modifyShard(myShard, myCollection, _))
      .Times(1);
  EXPECT_CALL(*stream, release).Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(stream.get());

  // DropShard
  entries.clear();
  entries.emplace_back(ReplicatedOperation::buildDropShardOperation(myShard));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*shardHandlerMock, dropShard(myShard)).Times(1);
  EXPECT_CALL(*stream, release).Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(stream.get());

  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
}

TEST_F(DocumentStateFollowerDeathTest,
       follower_dies_if_shard_creation_or_deletion_fails) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(ReplicatedOperation::buildCreateShardOperation(
      shardId, TRI_COL_TYPE_DOCUMENT, velocypack::SharedSlice()));
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  ON_CALL(*shardHandlerMock, ensureShard(shardId, TRI_COL_TYPE_DOCUMENT, _))
      .WillByDefault(Return(Result(TRI_ERROR_WAS_ERLAUBE)));
  ASSERT_DEATH_CORE_FREE(
      std::ignore =
          follower->applyEntries(std::move(entryIterator)).waitAndGet(),
      "");

  entries.clear();
  entries.emplace_back(ReplicatedOperation::buildDropShardOperation(shardId));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  ON_CALL(*shardHandlerMock, dropShard(shardId))
      .WillByDefault(Return(Result(TRI_ERROR_WAS_ERLAUBE)));
  ASSERT_DEATH_CORE_FREE(
      std::ignore =
          follower->applyEntries(std::move(entryIterator)).waitAndGet(),
      "");
}

TEST_F(DocumentStateFollowerTest, follower_ignores_invalid_transactions) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  // Try to apply a regular entry, but pretend the shard is not available
  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(createDocumentEntry(TransactionId{6}));
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(std::get<ReplicatedOperation::Insert>(
                  entries[0].getInnerOperation())))
      .Times(1)
      .WillOnce(Return(Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)));
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());

  // Try to commit the previous entry
  entries.clear();
  entries.emplace_back(
      ReplicatedOperation::buildCommitOperation(TransactionId{6}));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(std::get<ReplicatedOperation::Commit>(
                  entries[0].getInnerOperation())))
      .Times(0);
  // we do not actually commit anything, because the transaction is invalid, but
  // we still release the entry!
  EXPECT_CALL(*stream, release(LogIndex{1})).Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());

  // Try to apply another entry, this time making the shard available
  entries.clear();
  entries.emplace_back(createDocumentEntry(TransactionId{10}));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(std::get<ReplicatedOperation::Insert>(
                  entries[0].getInnerOperation())))
      .Times(1);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(shardHandlerMock.get());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
}

TEST_F(DocumentStateFollowerTest,
       follower_aborts_transactions_of_dropped_shard) {
  using namespace testing;

  auto transactionHandlerMock = createRealTransactionHandler();
  auto follower = createFollower();
  auto res = follower->acquireSnapshot("participantId");
  EXPECT_TRUE(res.isReady() && res.waitAndGet().ok());
  auto stream = follower->stream;

  std::vector<DocumentLogEntry> entries;
  entries.emplace_back(
      ReplicatedOperation{ReplicatedOperation::buildDocumentOperation(
          TRI_VOC_DOCUMENT_OPERATION_INSERT, TransactionId{6}, ShardID{1},
          velocypack::SharedSlice(), "root")});
  entries.emplace_back(
      ReplicatedOperation{ReplicatedOperation::buildDocumentOperation(
          TRI_VOC_DOCUMENT_OPERATION_INSERT, TransactionId{10}, ShardID{2},
          velocypack::SharedSlice(), "root")});
  auto entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);
  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());

  entries.clear();
  entries.emplace_back(
      ReplicatedOperation::buildDropShardOperation(ShardID{1}));
  entryIterator = std::make_unique<DocumentLogEntryIterator>(entries);

  ON_CALL(*transactionHandlerMock, getTransactionsForShard(ShardID{1}))
      .WillByDefault(Return(std::vector<TransactionId>{TransactionId{6}}));
  ON_CALL(*transactionHandlerMock, getTransactionsForShard(ShardID{2}))
      .WillByDefault(Return(std::vector<TransactionId>{TransactionId{10}}));
  EXPECT_CALL(*transactionHandlerMock, getTransactionsForShard(ShardID{1}))
      .Times(1);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(ReplicatedOperation::Abort{TransactionId{6}}))
      .Times(1);
  EXPECT_CALL(*transactionHandlerMock, getTransactionsForShard(ShardID{2}))
      .Times(0);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(ReplicatedOperation::Abort{TransactionId{10}}))
      .Times(0);
  EXPECT_CALL(*transactionHandlerMock,
              applyEntry(std::get<ReplicatedOperation::DropShard>(
                  entries[0].getInnerOperation())))
      .Times(1);
  EXPECT_CALL(*stream, release(LogIndex{1})).Times(1);

  res = follower->applyEntries(std::move(entryIterator));
  ASSERT_TRUE(res.waitAndGet().ok());
  Mock::VerifyAndClearExpectations(transactionHandlerMock.get());
  Mock::VerifyAndClearExpectations(stream.get());
}
