////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_ROCKSDB_ROCKSDB_TRANSACTION_STATE_H
#define ARANGOD_ROCKSDB_ROCKSDB_TRANSACTION_STATE_H 1

#include "Basics/Common.h"
#include "Basics/SmallVector.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Hints.h"
#include "Transaction/Methods.h"
#include "VocBase/AccessMode.h"
#include "VocBase/voc-types.h"

#include <rocksdb/options.h>
#include <rocksdb/status.h>

struct TRI_vocbase_t;

namespace rocksdb {
class Transaction;
class Slice;
class Iterator;
}  // namespace rocksdb

namespace arangodb {
namespace cache {
struct Transaction;
}
class LogicalCollection;
struct RocksDBDocumentOperation;
namespace transaction {
class Methods;
struct Options;
}
class TransactionCollection;
class RocksDBMethods;

/// @brief transaction type
class RocksDBTransactionState final : public TransactionState {
  friend class RocksDBMethods;
  friend class RocksDBReadOnlyMethods;
  friend class RocksDBTrxMethods;
  friend class RocksDBTrxUntrackedMethods;
  friend class RocksDBBatchedMethods;

 public:
  RocksDBTransactionState(TRI_vocbase_t* vocbase, transaction::Options const&);
  ~RocksDBTransactionState();

  /// @brief begin a transaction
  Result beginTransaction(transaction::Hints hints) override;

  /// @brief commit a transaction
  Result commitTransaction(transaction::Methods* trx) override;

  /// @brief abort a transaction
  Result abortTransaction(transaction::Methods* trx) override;

  uint64_t numInserts() const { return _numInserts; }
  uint64_t numUpdates() const { return _numUpdates; }
  uint64_t numRemoves() const { return _numRemoves; }

  /// @brief reset previous log state after a rollback to safepoint
  void resetLogState() { _lastUsedCollection = 0; }

  inline bool hasOperations() const {
    return (_numInserts > 0 || _numRemoves > 0 || _numUpdates > 0);
  }

  bool hasFailedOperations() const override {
    return (_status == transaction::Status::ABORTED) && hasOperations();
  }

  void prepareOperation(TRI_voc_cid_t collectionId, TRI_voc_rid_t revisionId,
                        StringRef const& key,
                        TRI_voc_document_operation_e operationType);

  /// @brief add an operation for a transaction collection
  RocksDBOperationResult addOperation(
      TRI_voc_cid_t collectionId, TRI_voc_rid_t revisionId,
      TRI_voc_document_operation_e operationType, uint64_t operationSize,
      uint64_t keySize);

  RocksDBMethods* rocksdbMethods();

  /// @brief insert a snapshot into a (not yet started) transaction.
  ///        Only ever valid on a read-only transaction
  void donateSnapshot(rocksdb::Snapshot const* snap);
  /// @brief steal snapshot of this transaction. Only ever valid on a
  ///        read-only transaction
  rocksdb::Snapshot const* stealSnapshot();
  
  /// @brief Rocksdb sequence number of snapshot. Works while trx
  ///        has either a snapshot or a transaction
  uint64_t sequenceNumber() const;

  static RocksDBTransactionState* toState(transaction::Methods* trx) {
    TRI_ASSERT(trx != nullptr);
    TransactionState* state = trx->state();
    TRI_ASSERT(state != nullptr);
    return static_cast<RocksDBTransactionState*>(state);
  }

  static RocksDBMethods* toMethods(transaction::Methods* trx) {
    TRI_ASSERT(trx != nullptr);
    TransactionState* state = trx->state();
    TRI_ASSERT(state != nullptr);
    return static_cast<RocksDBTransactionState*>(state)->rocksdbMethods();
  }

  /// @brief make some internal preparations for accessing this state in
  /// parallel from multiple threads. READ-ONLY transactions
  void prepareForParallelReads() { _parallel = true; }
  /// @brief in parallel mode. READ-ONLY transactions
  bool inParallelMode() const { return _parallel; }
  /// @brief temporarily lease a Builder object. Not thread safe
  RocksDBKey* leaseRocksDBKey();
  /// @brief return a temporary RocksDBKey object. Not thread safe
  void returnRocksDBKey(RocksDBKey* key);

 private:
  void createTransaction();
  arangodb::Result internalCommit();

 private:
  /// @brief rocksdb transaction may be null for read only transactions
  std::unique_ptr<rocksdb::Transaction> _rocksTransaction;
  /// @brief rocksdb snapshot, is null if _rocksTransaction is set
  rocksdb::Snapshot const* _snapshot;
  /// @brief write options used
  rocksdb::WriteOptions _rocksWriteOptions;
  ///@brief read options which must be used to guarantee isolation
  rocksdb::ReadOptions _rocksReadOptions;
  /// @brief cache transaction to unblock blacklisted keys
  cache::Transaction* _cacheTx;
  /// @brief wrapper to use outside this class to access rocksdb
  std::unique_ptr<RocksDBMethods> _rocksMethods;

  // if a transaction gets bigger than these values then an automatic
  // intermediate commit will be done
  uint64_t _numInserts;
  uint64_t _numUpdates;
  uint64_t _numRemoves;

  /// @brief Last collection used for transaction. Used for WAL
  TRI_voc_cid_t _lastUsedCollection;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  /// store the number of log entries in WAL
  uint64_t _numLogdata = 0;
#endif
  SmallVector<RocksDBKey*, 32>::allocator_type::arena_type _arena;
  SmallVector<RocksDBKey*, 32> _keys;
  /// @brief if true there key buffers will no longer be shared
  bool _parallel;
};

class RocksDBKeyLeaser {
 public:
  explicit RocksDBKeyLeaser(transaction::Methods*);
  ~RocksDBKeyLeaser();
  inline RocksDBKey* builder() const { return _key; }
  inline RocksDBKey* operator->() const { return _key; }
  inline RocksDBKey* get() const { return _key; }
  inline RocksDBKey& ref() const {return *_key; }
 private:
  RocksDBTransactionState* _rtrx;
  bool _parallel;
  RocksDBKey* _key;
  RocksDBKey _internal;
};

}  // namespace arangodb

#endif
