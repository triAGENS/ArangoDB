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

struct DocumentStateTransactionHandlerTest : DocumentStateMachineTest {
  auto createDocumentOperation(TRI_voc_document_operation_e operationType,
                               TransactionId tid) {
    return ReplicatedOperation::buildDocumentOperation(
        operationType, tid, shardId, velocypack::SharedSlice(), "root");
  }

  auto createTransactionHandler() {
    return DocumentStateTransactionHandler(GlobalLogIdentifier{dbName, logId},
                                           &vocbaseMock, handlersFactoryMock,
                                           shardHandlerMock);
  }
};

TEST_F(
    DocumentStateTransactionHandlerTest,
    test_transactionHandler_ensureTransaction_creates_new_transaction_only_once) {
  using namespace testing;

  auto transactionHandler = createTransactionHandler();
  auto tid = TransactionId{6};
  auto op = createDocumentOperation(TRI_VOC_DOCUMENT_OPERATION_UPDATE, tid);

  // Create transaction for the first time.
  EXPECT_CALL(
      *handlersFactoryMock,
      createTransaction(_, tid, shardId, AccessMode::Type::WRITE, "root"))
      .Times(1);
  auto res =
      std::visit([&](auto& o) { return transactionHandler.applyEntry(o); }, op);
  EXPECT_TRUE(res.ok()) << res;
  Mock::VerifyAndClearExpectations(handlersFactoryMock.get());
  EXPECT_EQ(transactionHandler.getUnfinishedTransactions().size(), 1);

  // Use an existing transaction ID and expect the transaction to be reused.
  EXPECT_CALL(*handlersFactoryMock, createTransaction(_, _, _, _, _)).Times(0);
  res =
      std::visit([&](auto& o) { return transactionHandler.applyEntry(o); }, op);
  EXPECT_TRUE(res.ok()) << res;
  Mock::VerifyAndClearExpectations(handlersFactoryMock.get());
  EXPECT_EQ(transactionHandler.getUnfinishedTransactions().size(), 1);
}

TEST_F(DocumentStateTransactionHandlerTest,
       test_transactionHandler_removeTransaction) {
  using namespace testing;

  auto transactionHandler = createTransactionHandler();
  auto tid = TransactionId{6};
  auto op = createDocumentOperation(TRI_VOC_DOCUMENT_OPERATION_UPDATE, tid);

  auto res = std::visit(
      [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
      op);
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(transactionHandler.getUnfinishedTransactions().size(), 1);
  transactionHandler.removeTransaction(tid);
  EXPECT_TRUE(transactionHandler.getUnfinishedTransactions().empty());
}

TEST_F(DocumentStateTransactionHandlerTest,
       test_transactionHandler_applyEntry_abortAll_clears_everything) {
  using namespace testing;

  auto transactionHandler = createTransactionHandler();
  auto tid = TransactionId{6};
  auto op = ReplicatedOperation{
      createDocumentOperation(TRI_VOC_DOCUMENT_OPERATION_REMOVE, tid)};

  auto res = std::visit<Result>(
      overload{
          [&](auto&& o) -> Result {
            return transactionHandler.applyEntry(std::move(o));
          },
          [&](ReplicatedOperation::CreateIndex&) -> Result {
            std::abort(); /* unused */
          },
      },
      op.operation);
  EXPECT_TRUE(res.ok()) << res;
  ASSERT_EQ(transactionHandler.getUnfinishedTransactions().size(), 1);

  op = ReplicatedOperation::buildAbortAllOngoingTrxOperation();
  res = std::visit(overload{
                       [&](auto&& o) -> Result {
                         return transactionHandler.applyEntry(std::move(o));
                       },
                       [&](ReplicatedOperation::CreateIndex&) -> Result {
                         std::abort(); /* unused */
                       },
                   },
                   op.operation);
  EXPECT_TRUE(res.ok()) << res;
  ASSERT_TRUE(transactionHandler.getUnfinishedTransactions().empty());
}

TEST_F(DocumentStateTransactionHandlerTest,
       test_applyEntry_apply_transaction_and_commit) {
  using namespace testing;

  auto transactionHandler = createTransactionHandler();
  auto tid = TransactionId{6};
  auto op = ReplicatedOperation{
      createDocumentOperation(TRI_VOC_DOCUMENT_OPERATION_INSERT, tid)};

  // Expect the transaction to be created and applied successfully
  EXPECT_CALL(*handlersFactoryMock, createTransaction(_, tid, shardId, _, _))
      .Times(1);
  EXPECT_CALL(*transactionMock, apply).Times(1);
  auto result = std::visit(
      overload{
          [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
          [&](ReplicatedOperation::CreateIndex& o) -> Result { std::abort(); },
      },
      op.operation);
  EXPECT_TRUE(result.ok()) << result;
  Mock::VerifyAndClearExpectations(handlersFactoryMock.get());
  Mock::VerifyAndClearExpectations(transactionMock.get());

  // An intermediate commit should not affect the transaction
  op = ReplicatedOperation::buildIntermediateCommitOperation(tid);
  result = std::visit(
      overload{
          [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
          [&](ReplicatedOperation::CreateIndex& o) -> Result { std::abort(); },
      },
      op.operation);
  EXPECT_TRUE(result.ok()) << result;
  Mock::VerifyAndClearExpectations(transactionMock.get());
  ASSERT_TRUE(transactionHandler.getUnfinishedTransactions().contains(
      TransactionId{6}));

  // After commit, expect the transaction to be removed
  op = ReplicatedOperation::buildCommitOperation(tid);
  result = std::visit(
      overload{
          [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
          [&](ReplicatedOperation::CreateIndex& o) -> Result { std::abort(); },
      },
      op.operation);
  EXPECT_TRUE(result.ok()) << result;
  Mock::VerifyAndClearExpectations(transactionMock.get());
  ASSERT_TRUE(transactionHandler.getUnfinishedTransactions().empty());
}

TEST_F(DocumentStateTransactionHandlerTest,
       test_applyEntry_apply_transaction_and_abort) {
  using namespace testing;

  auto transactionHandler = createTransactionHandler();
  auto tid = TransactionId{6};
  auto op = ReplicatedOperation{
      createDocumentOperation(TRI_VOC_DOCUMENT_OPERATION_INSERT, tid)};

  // Start a new transaction and then abort it.
  EXPECT_CALL(*handlersFactoryMock, createTransaction).Times(1);
  EXPECT_CALL(*transactionMock, apply).Times(1);
  auto res = std::visit(
      overload{
          [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
          [&](ReplicatedOperation::CreateIndex& o) -> Result { std::abort(); },
      },
      op.operation);
  EXPECT_TRUE(res.ok()) << res;
  ASSERT_TRUE(transactionHandler.getUnfinishedTransactions().contains(tid));
  Mock::VerifyAndClearExpectations(transactionMock.get());
  Mock::VerifyAndClearExpectations(handlersFactoryMock.get());

  // Expect the transaction to be removed after abort
  op = ReplicatedOperation::buildAbortOperation(tid);
  res = std::visit(
      overload{
          [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
          [&](ReplicatedOperation::CreateIndex& o) -> Result { std::abort(); },
      },
      op.operation);
  EXPECT_TRUE(res.ok()) << res;
  Mock::VerifyAndClearExpectations(transactionMock.get());
  ASSERT_TRUE(transactionHandler.getUnfinishedTransactions().empty());
}

TEST_F(DocumentStateTransactionHandlerTest, test_applyEntry_handle_errors) {
  using namespace testing;

  auto transactionHandler = createTransactionHandler();
  auto tid = TransactionId{6};
  auto op = createDocumentOperation(TRI_VOC_DOCUMENT_OPERATION_INSERT, tid);

  // OperationResult failed, transaction should fail
  EXPECT_CALL(*transactionMock, apply(_))
      .WillOnce(Return(
          OperationResult{Result{TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION},
                          OperationOptions{}}));
  auto result = std::visit(
      [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
      op);
  EXPECT_TRUE(result.fail());
  Mock::VerifyAndClearExpectations(transactionMock.get());

  // Unique constraint violation, should not fail
  EXPECT_CALL(*transactionMock, apply(_))
      .WillOnce([](ReplicatedOperation::OperationType const&) {
        auto opRes = OperationResult{Result{}, OperationOptions{}};
        opRes.countErrorCodes[TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED] = 1;
        return opRes;
      });
  result = std::visit(
      [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
      op);
  EXPECT_TRUE(result.ok()) << result;
  Mock::VerifyAndClearExpectations(transactionMock.get());

  // DOCUMENT_NOT_FOUND error, should not fail
  EXPECT_CALL(*transactionMock, apply(_))
      .WillOnce([](ReplicatedOperation::OperationType const&) {
        auto opRes = OperationResult{Result{}, OperationOptions{}};
        opRes.countErrorCodes[TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND] = 1;
        return opRes;
      });
  result = std::visit(
      [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
      op);
  EXPECT_TRUE(result.ok()) << result;
  Mock::VerifyAndClearExpectations(transactionMock.get());

  // An error inside countErrorCodes, transaction should fail
  EXPECT_CALL(*transactionMock, apply(_))
      .WillOnce([](ReplicatedOperation::OperationType const&) {
        auto opRes = OperationResult{Result{}, OperationOptions{}};
        opRes.countErrorCodes[TRI_ERROR_TRANSACTION_DISALLOWED_OPERATION] = 1;
        return opRes;
      });
  result = std::visit(
      [&](auto&& o) { return transactionHandler.applyEntry(std::move(o)); },
      op);
  ASSERT_TRUE(result.fail());
  Mock::VerifyAndClearExpectations(transactionMock.get());
}

TEST(ActiveTransactionsQueueTest,
     test_activeTransactions_releaseIndex_calculation) {
  auto activeTrx = ActiveTransactionsQueue{};

  ASSERT_EQ(activeTrx.getReleaseIndex(), std::nullopt);
  activeTrx.markAsActive(TransactionId{100}, LogIndex{100});
  ASSERT_EQ(activeTrx.getTransactions().size(), 1);
  activeTrx.markAsInactive(TransactionId{100});
  ASSERT_EQ(activeTrx.getTransactions().size(), 0);
  ASSERT_EQ(activeTrx.getReleaseIndex(), std::nullopt);

  activeTrx.markAsActive(TransactionId{200}, LogIndex{200});
  activeTrx.markAsActive(TransactionId{300}, LogIndex{300});
  activeTrx.markAsActive(TransactionId{400}, LogIndex{400});
  ASSERT_EQ(activeTrx.getTransactions().size(), 3);

  activeTrx.markAsInactive(TransactionId{200});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{299});
  activeTrx.markAsInactive(TransactionId{400});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{299});
  activeTrx.markAsInactive(TransactionId{300});
  ASSERT_EQ(activeTrx.getReleaseIndex(), std::nullopt);

  activeTrx.markAsActive(TransactionId{500}, LogIndex{500});
  ASSERT_EQ(activeTrx.getTransactions().size(), 1);
  activeTrx.clear();
  ASSERT_EQ(activeTrx.getTransactions().size(), 0);

  activeTrx.markAsActive(LogIndex{600});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{599});
  activeTrx.markAsActive(TransactionId{700}, LogIndex{700});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{599});
  activeTrx.markAsActive(LogIndex{800});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{599});
  activeTrx.markAsInactive(LogIndex{800});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{599});
  activeTrx.markAsInactive(LogIndex{600});
  ASSERT_EQ(activeTrx.getReleaseIndex(), LogIndex{699});
  activeTrx.markAsInactive(TransactionId{700});
  ASSERT_EQ(activeTrx.getReleaseIndex(), std::nullopt);
}

TEST(ActiveTransactionsQueueDeathTest, test_activeTransactions_death) {
  auto activeTrx = ActiveTransactionsQueue{};
  activeTrx.markAsActive(TransactionId{100}, LogIndex{100});
  ASSERT_DEATH_CORE_FREE(activeTrx.markAsActive(LogIndex{99}), "");
}
