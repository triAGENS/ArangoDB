////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <immer/flex_vector_transient.hpp>

#include "Basics/Result.h"
#include "Replication2/ReplicatedLog/Components/StorageManager.h"
#include "Replication2/ReplicatedState/PersistedStateInfo.h"
#include "Replication2/Mocks/FakeStorageEngineMethods.h"
#include "Replication2/Mocks/FakeAsyncExecutor.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;
using namespace arangodb::replication2::replicated_state;

struct StorageManagerTest : ::testing::Test {
  std::uint64_t const objectId = 1;
  LogId const logId = LogId{12};
  std::shared_ptr<test::DelayedExecutor> executor =
      std::make_shared<test::DelayedExecutor>();
  test::FakeStorageEngineMethodsContext methods{
      objectId, logId, executor, LogRange{LogIndex{1}, LogIndex{100}}};
  std::shared_ptr<StorageManager> storageManager =
      std::make_shared<StorageManager>(methods.getMethods());
};

TEST_F(StorageManagerTest, transaction_resign) {
  auto trx = storageManager->transaction();
  trx.reset();
  auto methods = storageManager->resign();
}

TEST_F(StorageManagerTest, transaction_resign_transaction) {
  auto trx = storageManager->transaction();
  trx.reset();
  auto methods = storageManager->resign();
  EXPECT_ANY_THROW({ std::ignore = storageManager->transaction(); });
}

TEST_F(StorageManagerTest, transaction_remove_front) {
  auto trx = storageManager->transaction();
  auto f = trx->removeFront(LogIndex{50});

  EXPECT_FALSE(f.isReady());
  executor->runOnce();
  ASSERT_TRUE(f.isReady());

  EXPECT_EQ(methods.log.size(), 50);  // [50, 100)
  EXPECT_EQ(methods.log.begin()->first, LogIndex{50});
  EXPECT_EQ(methods.log.rbegin()->first, LogIndex{99});

  auto trx2 = storageManager->transaction();
  auto logBounds = trx2->getLogBounds();
  EXPECT_EQ(logBounds, (LogRange{LogIndex{50}, LogIndex{100}}));
}

TEST_F(StorageManagerTest, transaction_remove_back) {
  auto trx = storageManager->transaction();
  auto f = trx->removeBack(LogIndex{50});

  EXPECT_FALSE(f.isReady());
  executor->runOnce();
  ASSERT_TRUE(f.isReady());

  EXPECT_EQ(methods.log.size(), 49);  // [1, 50)
  EXPECT_EQ(methods.log.begin()->first, LogIndex{1});
  EXPECT_EQ(methods.log.rbegin()->first, LogIndex{49});

  auto trx2 = storageManager->transaction();
  auto logBounds = trx2->getLogBounds();
  EXPECT_EQ(logBounds, (LogRange{LogIndex{1}, LogIndex{49}}));
}

TEST_F(StorageManagerTest, concurrent_remove_front_back) {
  auto f1 = [&] {
    auto trx = storageManager->transaction();
    return trx->removeBack(LogIndex{70});
  }();

  auto f2 = [&] {
    auto trx = storageManager->transaction();
    return trx->removeFront(LogIndex{40});
  }();

  EXPECT_FALSE(f1.isReady());
  EXPECT_FALSE(f2.isReady());
  executor->runAll();
  ASSERT_TRUE(f1.isReady());
  ASSERT_TRUE(f2.isReady());

  EXPECT_EQ(methods.log.size(), 30);  // [40, 70)
  EXPECT_EQ(methods.log.begin()->first, LogIndex{40});
  EXPECT_EQ(methods.log.rbegin()->first, LogIndex{69});

  auto trx2 = storageManager->transaction();
  auto logBounds = trx2->getLogBounds();
  EXPECT_EQ(logBounds, (LogRange{LogIndex{40}, LogIndex{69}}));
}

namespace {
auto makeRange(LogTerm term, LogRange range) -> InMemoryLog {
  InMemoryLog::log_type log;
  auto transient = log.transient();
  for (auto idx : range) {
    transient.push_back(InMemoryLogEntry{
        PersistingLogEntry{term, idx, LogPayload::createFromString("")}});
  }
  return InMemoryLog(transient.persistent());
}
}  // namespace

TEST_F(StorageManagerTest, transaction_append) {
  auto trx = storageManager->transaction();
  auto f =
      trx->appendEntries(makeRange(LogTerm{1}, {LogIndex{100}, LogIndex{120}}));

  EXPECT_FALSE(f.isReady());
  executor->runOnce();
  ASSERT_TRUE(f.isReady());

  EXPECT_EQ(methods.log.size(), 119);  // [1, 120)
  EXPECT_EQ(methods.log.begin()->first, LogIndex{1});
  EXPECT_EQ(methods.log.rbegin()->first, LogIndex{119});

  auto trx2 = storageManager->transaction();
  auto logBounds = trx2->getLogBounds();
  EXPECT_EQ(logBounds, (LogRange{LogIndex{1}, LogIndex{120}}));
}

struct StorageEngineMethodsGMock : IStorageEngineMethods {
  MOCK_METHOD(Result, updateMetadata, (PersistedStateInfo), (override));
  MOCK_METHOD(ResultT<PersistedStateInfo>, readMetadata, (), (override));
  MOCK_METHOD(std::unique_ptr<PersistedLogIterator>, read, (LogIndex),
              (override));
  MOCK_METHOD(futures::Future<ResultT<SequenceNumber>>, insert,
              (std::unique_ptr<PersistedLogIterator>, WriteOptions const&),
              (override));
  MOCK_METHOD(futures::Future<ResultT<SequenceNumber>>, removeFront,
              (LogIndex stop, WriteOptions const&), (override));
  MOCK_METHOD(futures::Future<ResultT<SequenceNumber>>, removeBack,
              (LogIndex start, WriteOptions const&), (override));
  MOCK_METHOD(std::uint64_t, getObjectId, (), (override));
  MOCK_METHOD(LogId, getLogId, (), (override));
  MOCK_METHOD(SequenceNumber, getSyncedSequenceNumber, (), (override));
  MOCK_METHOD(futures::Future<futures::Unit>, waitForSync, (SequenceNumber),
              (override));
};

struct StorageEngineMethodsMockFactory {
  auto create() -> std::unique_ptr<StorageEngineMethodsGMock> {
    auto ptr = std::make_unique<StorageEngineMethodsGMock>();
    lastPtr = ptr.get();

    EXPECT_CALL(*ptr, read).Times(1).WillOnce([](LogIndex idx) {
      auto log = makeRange(LogTerm{1}, {LogIndex{10}, LogIndex{100}});
      return log.getPersistedLogIterator();
    });

    return ptr;
  }
  auto operator->() noexcept -> StorageEngineMethodsGMock* { return lastPtr; }
  auto operator*() noexcept -> StorageEngineMethodsGMock& { return *lastPtr; }

 private:
  StorageEngineMethodsGMock* lastPtr{nullptr};
};

struct StorageManagerGMockTest : ::testing::Test {
  StorageEngineMethodsMockFactory methods;

  std::shared_ptr<StorageManager> storageManager =
      std::make_shared<StorageManager>(methods.create());

  using StorageEngineFuture =
      futures::Promise<ResultT<IStorageEngineMethods::SequenceNumber>>;
};

TEST_F(StorageManagerGMockTest, multiple_actions_with_error) {
  std::optional<StorageEngineFuture> p1;

  EXPECT_CALL(*methods, removeFront)
      .Times(1)
      .WillOnce(
          [&p1](LogIndex stop, IStorageEngineMethods::WriteOptions const&) {
            return p1.emplace().getFuture();
          });

  auto trx = storageManager->transaction();
  auto f1 = trx->removeFront(LogIndex(20));

  auto trx2 = storageManager->transaction();
  auto f2 = trx2->removeBack(LogIndex{80});

  EXPECT_FALSE(f1.isReady());
  EXPECT_FALSE(f2.isReady());

  // resolve first promise with error
  p1->setValue(Result{TRI_ERROR_DEBUG});

  // first one failed with original error
  ASSERT_TRUE(f1.isReady());
  EXPECT_EQ(f1.get().errorNumber(), TRI_ERROR_DEBUG);

  // others are aborted due to conflict
  ASSERT_TRUE(f2.isReady());
  EXPECT_EQ(f2.get().errorNumber(), TRI_ERROR_ARANGO_CONFLICT);
}
