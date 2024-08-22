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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/ReadWriteLock.h"
#include "ClusterEngine/ClusterSelectivityEstimates.h"
#include "ClusterEngine/Common.h"
#include "StorageEngine/PhysicalCollection.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/LogicalCollection.h"

namespace rocksdb {
class Transaction;
}

namespace arangodb {
namespace cache {
class Cache;
}

class LogicalCollection;
class Result;
class LocalDocumentId;

class ClusterCollection final : public PhysicalCollection {
 public:
  explicit ClusterCollection(LogicalCollection& collection,
                             ClusterEngineType engineType,
                             velocypack::Slice info);

  ~ClusterCollection();

  /// @brief fetches current index selectivity estimates
  /// if allowUpdate is true, will potentially make a cluster-internal roundtrip
  /// to fetch current values!
  IndexEstMap clusterIndexEstimates(bool allowUpdating,
                                    TransactionId tid) override;

  /// @brief flushes the current index selectivity estimates
  void flushClusterIndexEstimates() override;

  Result updateProperties(velocypack::Slice slice) override;

  /// @brief export properties
  void getPropertiesVPack(velocypack::Builder&) const override;

  /// @brief return the figures for a collection
  futures::Future<OperationResult> figures(
      bool details, OperationOptions const& options) override;

  RevisionId revision(transaction::Methods* trx) const override;
  uint64_t numberDocuments(transaction::Methods* trx) const override;

  bool cacheEnabled() const noexcept override;

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

  std::unique_ptr<IndexIterator> getSortedAllIterator(
      transaction::Methods* trx) const;

  ////////////////////////////////////
  // -- SECTION DML Operations --
  ///////////////////////////////////

  Result truncate(transaction::Methods& trx, OperationOptions& options,
                  bool& usedRangeDelete) override;

  void deferDropCollection(
      std::function<bool(LogicalCollection&)> const& callback) override;

  Result lookupKey(transaction::Methods* trx, std::string_view key,
                   std::pair<LocalDocumentId, RevisionId>& result,
                   ReadOwnWrites) const override;

  Result lookupKeyForUpdate(
      transaction::Methods* trx, std::string_view key,
      std::pair<LocalDocumentId, RevisionId>& result) const override;

  Result lookup(transaction::Methods* trx, std::string_view key,
                IndexIterator::DocumentCallback const& cb,
                LookupOptions options) const final;

  Result lookup(transaction::Methods* trx, LocalDocumentId token,
                IndexIterator::DocumentCallback const& cb,
                LookupOptions options,
                StorageSnapshot const* snapshot = nullptr) const final;

  Result lookup(transaction::Methods* trx, std::span<LocalDocumentId> tokens,
                MultiDocumentCallback const& cb,
                LookupOptions options) const final;

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

 protected:
  /// @brief Inject figures that are specific to StorageEngine
  void figuresSpecific(bool details, velocypack::Builder&) override;

 private:
  // keep locks just to adhere to behavior in other collections
  mutable basics::ReadWriteLock _exclusiveLock;
  ClusterEngineType _engineType;
  velocypack::Builder _info;
  ClusterSelectivityEstimates _selectivityEstimates;
};

}  // namespace arangodb
