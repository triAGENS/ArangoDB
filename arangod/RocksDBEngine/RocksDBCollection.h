////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Statistics/ServerStatistics.h"
#include "RocksDBEngine/RocksDBMetaCollection.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/vocbase.h"

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
class RocksDBPrimaryIndex;
class RocksDBSavePoint;
class LocalDocumentId;

class RocksDBCollection final : public RocksDBMetaCollection {
  friend class RocksDBEngine;

  RocksDBCollection(LogicalCollection& collection,
                    PhysicalCollection const*);  // use in cluster only!!!!!

 public:
  explicit RocksDBCollection(LogicalCollection& collection,
                             velocypack::Slice info);
  ~RocksDBCollection();

  arangodb::Result updateProperties(velocypack::Slice slice) override;

  virtual PhysicalCollection* clone(LogicalCollection& logical) const override;

  /// @brief export properties
  void getPropertiesVPack(velocypack::Builder&) const override;

  /// @brief closes an open collection
  ErrorCode close() override;

  /// return bounds for all documents
  RocksDBKeyBounds bounds() const override;

  ////////////////////////////////////
  // -- SECTION Indexes --
  ///////////////////////////////////

  void prepareIndexes(velocypack::Slice indexesSlice) override;

  std::shared_ptr<Index> createIndex(velocypack::Slice info, bool restore,
                                     bool& created) override;

  Result updateIndex(IndexId iid, velocypack::Slice body,
                     VPackBuilder& updated) override;

  /// @brief Drop an index with the given iid.
  bool dropIndex(IndexId iid) override;

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

  bool lookupRevision(transaction::Methods* trx, velocypack::Slice const& key,
                      RevisionId& revisionId, ReadOwnWrites) const;

  Result read(transaction::Methods*, std::string_view key,
              IndexIterator::DocumentCallback const& cb,
              ReadOwnWrites readOwnWrites) const override;

  /// @brief lookup with callback, not thread-safe on same transaction::Context
  Result read(transaction::Methods* trx, LocalDocumentId const& token,
              IndexIterator::DocumentCallback const& cb,
              ReadOwnWrites readOwnWrites) const override;

  Result insert(transaction::Methods& trx, RevisionId newRevisionId,
                velocypack::Slice newDocument,
                OperationOptions const& options) override;

  Result update(transaction::Methods& trx, LocalDocumentId previousDocumentId,
                RevisionId previousRevisionId,
                velocypack::Slice previousDocument, RevisionId newRevisionId,
                velocypack::Slice newDocument,
                OperationOptions const& options) override;

  Result replace(transaction::Methods& trx, LocalDocumentId previousDocumentId,
                 RevisionId previousRevisionId,
                 velocypack::Slice previousDocument, RevisionId newRevisionId,
                 velocypack::Slice newDocument,
                 OperationOptions const& options) override;

  Result remove(transaction::Methods& trx, LocalDocumentId previousDocumentId,
                RevisionId previousRevisionId,
                velocypack::Slice previousDocument,
                OperationOptions const& options) override;

  inline bool cacheEnabled() const { return _cacheEnabled; }

  bool hasDocuments() override;

  /// @brief lookup document in cache and / or rocksdb
  /// @param readCache attempt to read from cache
  /// @param fillCache fill cache with found document
  Result lookupDocument(transaction::Methods& trx, LocalDocumentId documentId,
                        velocypack::Builder& builder, bool readCache,
                        bool fillCache,
                        ReadOwnWrites readOwnWrites) const override;

 private:
  // optimized truncate, using DeleteRange operations.
  // this can only be used if the truncate is performed as a standalone
  // operation (i.e. not part of a larger transaction)
  Result truncateWithRangeDelete(transaction::Methods& trx);

  // slow truncate that performs a document-by-document removal.
  Result truncateWithRemovals(transaction::Methods& trx,
                              OperationOptions& options);

  [[nodiscard]] Result performUpdateOrReplace(
      transaction::Methods& trx, LocalDocumentId previousDocumentId,
      RevisionId previousRevisionId, velocypack::Slice previousDocument,
      RevisionId newRevisionId, velocypack::Slice newDocument,
      OperationOptions const& options, TRI_voc_document_operation_e opType);

  /// @brief return engine-specific figures
  void figuresSpecific(bool details, velocypack::Builder&) override;

  // @brief return the primary index
  // WARNING: Make sure that this instance
  // is somehow protected. If it goes out of all scopes
  // or it's indexes are freed the pointer returned will get invalidated.
  arangodb::RocksDBPrimaryIndex* primaryIndex() const {
    TRI_ASSERT(_primaryIndex != nullptr);
    return _primaryIndex;
  }

  arangodb::Result insertDocument(arangodb::transaction::Methods* trx,
                                  RocksDBSavePoint& savepoint,
                                  LocalDocumentId documentId,
                                  arangodb::velocypack::Slice doc,
                                  OperationOptions const& options,
                                  RevisionId revisionId) const;

  arangodb::Result removeDocument(arangodb::transaction::Methods* trx,
                                  RocksDBSavePoint& savepoint,
                                  LocalDocumentId documentId,
                                  arangodb::velocypack::Slice doc,
                                  OperationOptions const& options,
                                  RevisionId revisionId) const;

  arangodb::Result modifyDocument(
      transaction::Methods* trx, RocksDBSavePoint& savepoint,
      LocalDocumentId oldDocumentId, velocypack::Slice oldDoc,
      LocalDocumentId newDocumentId, velocypack::Slice newDoc,
      RevisionId oldRevisionId, RevisionId newRevisionId,
      OperationOptions const& options) const;

  /// @brief lookup document in cache and / or rocksdb
  /// @param readCache attempt to read from cache
  /// @param fillCache fill cache with found document
  arangodb::Result lookupDocumentVPack(transaction::Methods* trx,
                                       LocalDocumentId const& documentId,
                                       rocksdb::PinnableSlice& ps,
                                       bool readCache, bool fillCache,
                                       ReadOwnWrites readOwnWrites) const;

  Result lookupDocumentVPack(transaction::Methods*,
                             LocalDocumentId const& documentId,
                             IndexIterator::DocumentCallback const& cb,
                             bool withCache, ReadOwnWrites readOwnWrites) const;

  /// @brief create hash-cache
  void setupCache() const;
  /// @brief destory hash-cache
  void destroyCache() const;

  /// is this collection using a cache
  inline bool useCache() const noexcept { return (_cacheEnabled && _cache); }

  /// @brief track key in file
  void invalidateCacheEntry(RocksDBKey const& key) const;

  /// @brief can use non transactional range delete in write ahead log
  bool canUseRangeDeleteInWal() const;

 private:
  /// @brief cached ptr to primary index for performance, never delete
  RocksDBPrimaryIndex* _primaryIndex;

  // we have to store the cacheManager's pointer here because the
  // vocbase might already be destroyed at the time the destructor is executed
  cache::Manager* _cacheManager;

  /// @brief document cache (optional)
  mutable std::shared_ptr<cache::Cache> _cache;

  std::atomic<bool> _cacheEnabled;

  arangodb::TransactionStatistics& _statistics;
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
