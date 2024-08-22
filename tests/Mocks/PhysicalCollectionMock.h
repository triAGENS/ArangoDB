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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/Result.h"
#include "StorageEngine/PhysicalCollection.h"

#include <atomic>
#include <string_view>

class PhysicalCollectionMock : public arangodb::PhysicalCollection {
 public:
  struct DocElement {
    DocElement(std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> data,
               uint64_t docId);

    arangodb::velocypack::Slice data() const;
    std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> rawData() const;
    arangodb::LocalDocumentId docId() const;
    uint8_t const* vptr() const;
    void swapBuffer(
        std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>>& newData);

   private:
    std::shared_ptr<arangodb::velocypack::Buffer<uint8_t>> _data;
    uint64_t const _docId;
  };

  static std::function<void()> before;

  PhysicalCollectionMock(arangodb::LogicalCollection& collection);
  arangodb::futures::Future<std::shared_ptr<arangodb::Index>> createIndex(
      arangodb::velocypack::Slice info, bool restore, bool& created,
      std::shared_ptr<std::function<arangodb::Result(double)>> = nullptr,
      arangodb::replication2::replicated_state::document::Replication2Callback
          replicationCb = nullptr) override;
  void deferDropCollection(
      std::function<bool(arangodb::LogicalCollection&)> const& callback)
      override;
  arangodb::Result dropIndex(arangodb::IndexId iid) override;
  void figuresSpecific(bool details, arangodb::velocypack::Builder&) override;
  std::unique_ptr<arangodb::IndexIterator> getAllIterator(
      arangodb::transaction::Methods* trx,
      arangodb::ReadOwnWrites readOwnWrites) const override;
  std::unique_ptr<arangodb::IndexIterator> getAnyIterator(
      arangodb::transaction::Methods* trx) const override;
  std::unique_ptr<arangodb::ReplicationIterator> getReplicationIterator(
      arangodb::ReplicationIterator::Ordering, uint64_t) override;
  void getPropertiesVPack(arangodb::velocypack::Builder&) const override;
  arangodb::Result insert(arangodb::transaction::Methods& trx,
                          arangodb::IndexesSnapshot const& indexesSnapshot,
                          arangodb::RevisionId newRevisionId,
                          arangodb::velocypack::Slice newDocument,
                          arangodb::OperationOptions const& options) override;

  arangodb::Result lookupKey(
      arangodb::transaction::Methods*, std::string_view,
      std::pair<arangodb::LocalDocumentId, arangodb::RevisionId>&,
      arangodb::ReadOwnWrites) const override;
  arangodb::Result lookupKeyForUpdate(
      arangodb::transaction::Methods*, std::string_view,
      std::pair<arangodb::LocalDocumentId, arangodb::RevisionId>&)
      const override;
  uint64_t numberDocuments(arangodb::transaction::Methods* trx) const override;
  void prepareIndexes(arangodb::velocypack::Slice indexesSlice) override;

  arangodb::IndexEstMap clusterIndexEstimates(
      bool allowUpdating, arangodb::TransactionId tid) override;

  arangodb::Result lookup(arangodb::transaction::Methods* trx,
                          std::string_view key,
                          arangodb::IndexIterator::DocumentCallback const& cb,
                          LookupOptions options) const final;
  arangodb::Result lookup(
      arangodb::transaction::Methods* trx, arangodb::LocalDocumentId token,
      arangodb::IndexIterator::DocumentCallback const& cb,
      LookupOptions options,
      arangodb::StorageSnapshot const* snapshot) const final;
  arangodb::Result lookup(arangodb::transaction::Methods* trx,
                          std::span<arangodb::LocalDocumentId> tokens,
                          MultiDocumentCallback const& cb,
                          LookupOptions options) const final;
  arangodb::Result remove(arangodb::transaction::Methods& trx,
                          arangodb::IndexesSnapshot const& indexesSnapshot,
                          arangodb::LocalDocumentId previousDocumentId,
                          arangodb::RevisionId previousRevisionId,
                          arangodb::velocypack::Slice previousDocument,
                          arangodb::OperationOptions const& options) override;
  arangodb::Result replace(arangodb::transaction::Methods& trx,
                           arangodb::IndexesSnapshot const& indexesSnapshot,
                           arangodb::LocalDocumentId newDocumentId,
                           arangodb::RevisionId previousRevisionId,
                           arangodb::velocypack::Slice previousDocument,
                           arangodb::RevisionId newRevisionId,
                           arangodb::velocypack::Slice newDocument,
                           arangodb::OperationOptions const& options) override;
  arangodb::RevisionId revision(
      arangodb::transaction::Methods* trx) const override;
  arangodb::Result truncate(arangodb::transaction::Methods& trx,
                            arangodb::OperationOptions& options,
                            bool& usedRangeDelete) override;
  void compact() override {}
  arangodb::Result update(arangodb::transaction::Methods& trx,
                          arangodb::IndexesSnapshot const& indexesSnapshot,
                          arangodb::LocalDocumentId newDocumentId,
                          arangodb::RevisionId previousRevisionId,
                          arangodb::velocypack::Slice previousDocument,
                          arangodb::RevisionId newRevisionId,
                          arangodb::velocypack::Slice newDocument,
                          arangodb::OperationOptions const& options) override;
  arangodb::Result updateProperties(arangodb::velocypack::Slice slice) override;

  bool cacheEnabled() const noexcept override { return false; }

 private:
  bool addIndex(std::shared_ptr<arangodb::Index> idx);

  arangodb::Result updateInternal(arangodb::transaction::Methods& trx,
                                  arangodb::LocalDocumentId newDocumentId,
                                  arangodb::RevisionId previousRevisionId,
                                  arangodb::velocypack::Slice previousDocument,
                                  arangodb::RevisionId newRevisionId,
                                  arangodb::velocypack::Slice newDocument,
                                  arangodb::OperationOptions const& options,
                                  bool isUpdate);

  uint64_t _lastDocumentId;
  // map _key => data. Keyslice references memory in the value
  std::unordered_map<std::string_view, DocElement> _documents;
};
