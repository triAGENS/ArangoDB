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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Statistics/ServerStatistics.h"
#include "RocksDBEngine/RocksDBMetaCollection.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
#include "VocBase/Identifiers/IndexId.h"

#include <atomic>
#include <memory>

namespace rocksdb {
class PinnableSlice;
class Transaction;
}  // namespace rocksdb

namespace arangodb {
namespace cache {
class Cache;
class Manager;
}  // namespace cache

class LogicalCollection;
class RocksDBSavePoint;
class LocalDocumentId;

class RocksDBCollection final : public RocksDBMetaCollection {
  friend class RocksDBEngine;

 public:
  explicit RocksDBCollection(LogicalCollection& collection,
                             velocypack::Slice info);
  ~RocksDBCollection();

  void deferDropCollection(
      std::function<bool(LogicalCollection&)> const& cb) override final;

  Result updateProperties(velocypack::Slice slice) override;

  /// @brief export properties
  void getPropertiesVPack(velocypack::Builder&) const override;

  /// return bounds for all documents
  RocksDBKeyBounds bounds() const override;

  ////////////////////////////////////
  // -- SECTION Indexes --
  ///////////////////////////////////

  futures::Future<std::shared_ptr<Index>> createIndex(
      velocypack::Slice info, bool restore, bool& created,
      std::shared_ptr<std::function<arangodb::Result(double)>> = nullptr,
      replication2::replicated_state::document::Replication2Callback
          replicationCb = nullptr) override;

  std::unique_ptr<IndexIterator> getAllIterator(
      transaction::Methods* trx, ReadOwnWrites readOwnWrites) const override;
  std::unique_ptr<IndexIterator> getAnyIterator(
      transaction::Methods* trx) const override;

  std::unique_ptr<ReplicationIterator> getReplicationIterator(
      ReplicationIterator::Ordering, uint64_t batchId) override;
  std::unique_ptr<ReplicationIterator> getReplicationIterator(
      ReplicationIterator::Ordering, transaction::Methods&) override;

  ////////////////////////////////////
  // -- SECTION DML Operations --
  ///////////////////////////////////

  Result truncate(transaction::Methods& trx, OperationOptions& options,
                  bool& usedRangeDelete) override;

  /// @brief returns the LocalDocumentId and the revision id for the document
  /// with the specified key.
  Result lookupKey(transaction::Methods* trx, std::string_view key,
                   std::pair<LocalDocumentId, RevisionId>& result,
                   ReadOwnWrites readOwnWrites) const override;

  /// @brief returns the LocalDocumentId and the revision id for the document
  /// with the specified key.
  Result lookupKeyForUpdate(
      transaction::Methods* trx, std::string_view key,
      std::pair<LocalDocumentId, RevisionId>& result) const override;

  bool lookupRevision(transaction::Methods* trx, velocypack::Slice key,
                      RevisionId& revisionId, ReadOwnWrites) const;

  Result lookup(transaction::Methods* trx, std::string_view key,
                IndexIterator::DocumentCallback const& cb,
                LookupOptions options) const override;

  Result lookup(transaction::Methods* trx, LocalDocumentId token,
                IndexIterator::DocumentCallback const& cb,
                LookupOptions options,
                StorageSnapshot const* snapshot = nullptr) const override;

  Result lookup(transaction::Methods* trx, std::span<LocalDocumentId> tokens,
                MultiDocumentCallback const& cb,
                LookupOptions options) const override;

  Result insert(transaction::Methods& trx,
                IndexesSnapshot const& indexesSnapshot,
                RevisionId newRevisionId, velocypack::Slice newDocument,
                OperationOptions const& options) override;

  Result update(transaction::Methods& trx,
                IndexesSnapshot const& indexesSnapshot,
                LocalDocumentId previousDocumentId,
                RevisionId previousRevisionId,
                velocypack::Slice previousDocument, RevisionId newRevisionId,
                velocypack::Slice newDocument,
                OperationOptions const& options) override;

  Result replace(transaction::Methods& trx,
                 IndexesSnapshot const& indexesSnapshot,
                 LocalDocumentId previousDocumentId,
                 RevisionId previousRevisionId,
                 velocypack::Slice previousDocument, RevisionId newRevisionId,
                 velocypack::Slice newDocument,
                 OperationOptions const& options) override;

  Result remove(transaction::Methods& trx,
                IndexesSnapshot const& indexesSnapshot,
                LocalDocumentId previousDocumentId,
                RevisionId previousRevisionId,
                velocypack::Slice previousDocument,
                OperationOptions const& options) override;

  bool cacheEnabled() const noexcept override;

  bool hasDocuments() override;

  void freeMemory() noexcept override;

  // @brief return the primary index
  // WARNING: Make sure that this instance
  // is somehow protected. If it goes out of all scopes
  // or its indexes are freed the pointer returned will get invalidated.
  RocksDBPrimaryIndex* primaryIndex() const override {
    TRI_ASSERT(_primaryIndex != nullptr);
    return _primaryIndex;
  }

 private:
  Result doLookupKey(transaction::Methods* trx, std::string_view key,
                     std::pair<LocalDocumentId, RevisionId>& result,
                     ReadOwnWrites readOwnWrites, bool lockForUpdate) const;

  // optimized truncate, using DeleteRange operations.
  // this can only be used if the truncate is performed as a standalone
  // operation (i.e. not part of a larger transaction)
  Result truncateWithRangeDelete(transaction::Methods& trx);

  // slow truncate that performs a document-by-document removal.
  Result truncateWithRemovals(transaction::Methods& trx,
                              OperationOptions& options);

  [[nodiscard]] Result performUpdateOrReplace(
      transaction::Methods& trx, IndexesSnapshot const& indexesSnapshot,
      LocalDocumentId previousDocumentId, RevisionId previousRevisionId,
      velocypack::Slice previousDocument, RevisionId newRevisionId,
      velocypack::Slice newDocument, OperationOptions const& options,
      TRI_voc_document_operation_e opType);

  /// @brief return engine-specific figures
  void figuresSpecific(bool details, velocypack::Builder&) override;

  Result insertDocument(transaction::Methods* trx,
                        IndexesSnapshot const& indexesSnapshot,
                        RocksDBSavePoint& savepoint, LocalDocumentId documentId,
                        velocypack::Slice doc, OperationOptions const& options,
                        RevisionId revisionId) const;

  Result removeDocument(transaction::Methods* trx,
                        IndexesSnapshot const& indexesSnapshot,
                        RocksDBSavePoint& savepoint, LocalDocumentId documentId,
                        velocypack::Slice doc, OperationOptions const& options,
                        RevisionId revisionId) const;

  Result modifyDocument(transaction::Methods* trx,
                        IndexesSnapshot const& indexesSnapshot,
                        RocksDBSavePoint& savepoint,
                        LocalDocumentId oldDocumentId, velocypack::Slice oldDoc,
                        LocalDocumentId newDocumentId, velocypack::Slice newDoc,
                        RevisionId oldRevisionId, RevisionId newRevisionId,
                        OperationOptions const& options) const;

  Result lookupDocumentVPack(transaction::Methods* trx, LocalDocumentId token,
                             IndexIterator::DocumentCallback const& cb,
                             LookupOptions options,
                             StorageSnapshot const* snapshot) const;

  /// @brief create hash-cache
  void setupCache() const;
  /// @brief destory hash-cache
  void destroyCache() const;

  /// @brief: this can return a nullptr. the caller has to check the result
  std::shared_ptr<cache::Cache> useCache() const noexcept;

  /// @brief track key in file
  void invalidateCacheEntry(RocksDBKey const& key) const;

  /// @brief can use non transactional range delete in write ahead log
  bool canUseRangeDeleteInWal() const;

 private:
  // callback that is called directly before the index is dropped.
  // the write-lock on all indexes is still held
  Result duringDropIndex(std::shared_ptr<Index> idx) override;

  // callback that is called directly after the index has been dropped.
  // no locks are held anymore.
  Result afterDropIndex(std::shared_ptr<Index> idx) override;

  // callback that is called while adding a new index. called under
  // indexes write-lock
  void duringAddIndex(std::shared_ptr<Index> idx) override;

  /// @brief cached ptr to primary index for performance, never delete
  RocksDBPrimaryIndex* _primaryIndex;

  // we have to store the cacheManager's pointer here because the
  // vocbase might already be destroyed at the time the destructor is executed
  cache::Manager* _cacheManager;

  // maximum size of cache values to be stored in in-memory cache for
  // documents.
  size_t const _maxCacheValueSize;

  /// @brief document cache (optional)
  /// use only with std::atomic_load|store_explicit()!
  mutable std::shared_ptr<cache::Cache> _cache;

  TransactionStatistics& _statistics;

  std::atomic_bool _cacheEnabled;
};

inline RocksDBCollection* toRocksDBCollection(PhysicalCollection* physical) {
  auto rv = static_cast<RocksDBCollection*>(physical);
  TRI_ASSERT(rv != nullptr);
  return rv;
}

inline RocksDBCollection* toRocksDBCollection(LogicalCollection& logical) {
  auto phys = logical.getPhysical();
  TRI_ASSERT(phys != nullptr);
  return toRocksDBCollection(phys);
}

}  // namespace arangodb
