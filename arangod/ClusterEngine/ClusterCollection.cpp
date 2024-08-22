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

#include "ClusterCollection.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ReadLocker.h"
#include "Basics/RecursiveLocker.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterMethods.h"
#include "ClusterEngine/ClusterEngine.h"
#include "ClusterEngine/ClusterIndex.h"
#include "Futures/Utilities.h"
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "Logger/LogMacros.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/IndexesSnapshot.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "VocBase/Identifiers/LocalDocumentId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/voc-types.h"

#include <velocypack/Collection.h>

namespace arangodb {

ClusterCollection::ClusterCollection(LogicalCollection& collection,
                                     ClusterEngineType engineType,
                                     velocypack::Slice info)
    : PhysicalCollection(collection),
      _engineType(engineType),
      _info(info),
      _selectivityEstimates(collection) {
  TRI_ASSERT(_engineType == ClusterEngineType::RocksDBEngine ||
             _engineType == ClusterEngineType::MockEngine);

#ifndef ARANGODB_USE_GOOGLE_TESTS
  TRI_ASSERT(_engineType == ClusterEngineType::RocksDBEngine);
#endif

  if (_engineType == ClusterEngineType::RocksDBEngine) {
    return;
  }
#ifdef ARANGODB_USE_GOOGLE_TESTS
  if (_engineType == ClusterEngineType::MockEngine) {
    return;
  }
#endif
  TRI_ASSERT(false);
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "invalid storage engine");
}

ClusterCollection::~ClusterCollection() = default;

/// @brief fetches current index selectivity estimates
/// if allowUpdate is true, will potentially make a cluster-internal roundtrip
/// to fetch current values!
IndexEstMap ClusterCollection::clusterIndexEstimates(bool allowUpdating,
                                                     TransactionId tid) {
  return _selectivityEstimates.get(allowUpdating, tid);
}

/// @brief flushes the current index selectivity estimates
void ClusterCollection::flushClusterIndexEstimates() {
  _selectivityEstimates.flush();
}

Result ClusterCollection::updateProperties(velocypack::Slice slice) {
  VPackBuilder merge;
  merge.openObject();

  if (_engineType == ClusterEngineType::RocksDBEngine) {
    bool def = basics::VelocyPackHelper::getBooleanValue(
        _info.slice(), StaticStrings::CacheEnabled, false);
    merge.add(StaticStrings::CacheEnabled,
              VPackValue(basics::VelocyPackHelper::getBooleanValue(
                  slice, StaticStrings::CacheEnabled, def)));

    if (VPackSlice schema = slice.get(StaticStrings::Schema);
        !schema.isNone()) {
      merge.add(StaticStrings::Schema, schema);
    }

    if (VPackSlice computedValues = slice.get(StaticStrings::ComputedValues);
        !computedValues.isNone()) {
      merge.add(StaticStrings::ComputedValues, computedValues);
    }
#ifdef ARANGODB_USE_GOOGLE_TESTS
  } else if (_engineType == ClusterEngineType::MockEngine) {
    // do nothing
#endif
  } else {
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid storage engine");
  }
  merge.close();
  TRI_ASSERT(merge.slice().isObject());
  TRI_ASSERT(merge.isClosed());

  TRI_ASSERT(_info.slice().isObject());
  TRI_ASSERT(_info.isClosed());

  VPackBuilder tmp = VPackCollection::merge(_info.slice(), merge.slice(), true);
  _info = std::move(tmp);

  TRI_ASSERT(_info.slice().isObject());
  TRI_ASSERT(_info.isClosed());

  // notify all indexes about the properties change for the collection
  auto indexesSnapshot = getIndexesSnapshot();
  auto const& indexes = indexesSnapshot.getIndexes();
  for (auto& idx : indexes) {
    // note: we have to exclude inverted indexes here,
    // as they are a different class type (no relationship to
    // ClusterIndex).
    if (idx->type() != Index::TRI_IDX_TYPE_INVERTED_INDEX &&
        idx->type() != Index::TRI_IDX_TYPE_IRESEARCH_LINK) {
      TRI_ASSERT(dynamic_cast<ClusterIndex*>(idx.get()) != nullptr);
      std::static_pointer_cast<ClusterIndex>(idx)->updateProperties(
          _info.slice());
    }
  }

  // nothing else to do
  return {};
}

/// @brief used for updating properties
void ClusterCollection::getPropertiesVPack(velocypack::Builder& result) const {
  TRI_ASSERT(result.isOpenObject());

  if (_engineType == ClusterEngineType::RocksDBEngine) {
    result.add(StaticStrings::CacheEnabled,
               VPackValue(basics::VelocyPackHelper::getBooleanValue(
                   _info.slice(), StaticStrings::CacheEnabled, false)));

    // note: computed values do not need to be handled here, as they are added
    // by LogicalCollection::appendVPack()
#ifdef ARANGODB_USE_GOOGLE_TESTS
  } else if (_engineType == ClusterEngineType::MockEngine) {
    // do nothing
#endif
  } else {
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid storage engine");
  }
}

/// @brief return the figures for a collection
futures::Future<OperationResult> ClusterCollection::figures(
    bool details, OperationOptions const& options) {
  auto& feature =
      _logicalCollection.vocbase().server().getFeature<ClusterFeature>();
  return figuresOnCoordinator(feature, _logicalCollection.vocbase().name(),
                              std::to_string(_logicalCollection.id().id()),
                              details, options);
}

void ClusterCollection::figuresSpecific(bool /*details*/,
                                        velocypack::Builder& /*builder*/) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);  // not used here
}

RevisionId ClusterCollection::revision(transaction::Methods* trx) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

uint64_t ClusterCollection::numberDocuments(transaction::Methods* trx) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

bool ClusterCollection::cacheEnabled() const noexcept {
  return basics::VelocyPackHelper::getBooleanValue(
      _info.slice(), StaticStrings::CacheEnabled, false);
}

futures::Future<std::shared_ptr<Index>> ClusterCollection::createIndex(
    velocypack::Slice info, bool restore, bool& created,
    std::shared_ptr<std::function<arangodb::Result(double)>> progress,
    replication2::replicated_state::document::Replication2Callback
        replicationCb) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());

  // prevent concurrent dropping
  WRITE_LOCKER(guard, _exclusiveLock);

  RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
  std::shared_ptr<Index> idx = lookupIndex(info);
  if (idx) {
    created = false;
    // We already have this index.
    co_return idx;
  }

  StorageEngine& engine = _logicalCollection.vocbase().engine();

  // We are sure that we do not have an index of this type.
  // We also hold the lock. Create it
  idx = engine.indexFactory().prepareIndexFromSlice(info, true,
                                                    _logicalCollection, false);
  TRI_ASSERT(idx != nullptr);

  // In the coordinator case we do not fill the index
  // We only inform the others.
  _indexes.emplace(idx);

  created = true;
  co_return idx;
}

std::unique_ptr<IndexIterator> ClusterCollection::getAllIterator(
    transaction::Methods* /*trx*/, ReadOwnWrites /*readOwnWrites*/) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

std::unique_ptr<IndexIterator> ClusterCollection::getAnyIterator(
    transaction::Methods* /*trx*/) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::truncate(transaction::Methods& /*trx*/,
                                   OperationOptions& /*options*/,
                                   bool& usedRangeDelete) {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::lookupKey(
    transaction::Methods* /*trx*/, std::string_view /*key*/,
    std::pair<LocalDocumentId, RevisionId>& /*result*/, ReadOwnWrites) const {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::lookupKeyForUpdate(
    transaction::Methods* /*trx*/, std::string_view /*key*/,
    std::pair<LocalDocumentId, RevisionId>& /*result*/) const {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::lookup(transaction::Methods* trx,
                                 std::string_view key,
                                 IndexIterator::DocumentCallback const& cb,
                                 LookupOptions options) const {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::lookup(transaction::Methods* trx,
                                 LocalDocumentId token,
                                 IndexIterator::DocumentCallback const& cb,
                                 LookupOptions options,
                                 StorageSnapshot const* snapshot) const {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::lookup(transaction::Methods* trx,
                                 std::span<LocalDocumentId> tokens,
                                 MultiDocumentCallback const& cb,
                                 LookupOptions options) const {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::insert(transaction::Methods&, IndexesSnapshot const&,
                                 RevisionId, velocypack::Slice,
                                 OperationOptions const&) {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::update(transaction::Methods&, IndexesSnapshot const&,
                                 LocalDocumentId, RevisionId, velocypack::Slice,
                                 RevisionId, velocypack::Slice,
                                 OperationOptions const&) {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::replace(transaction::Methods&, IndexesSnapshot const&,
                                  LocalDocumentId, RevisionId,
                                  velocypack::Slice, RevisionId,
                                  velocypack::Slice, OperationOptions const&) {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

Result ClusterCollection::remove(transaction::Methods&, IndexesSnapshot const&,
                                 LocalDocumentId, RevisionId, velocypack::Slice,
                                 OperationOptions const&) {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}

void ClusterCollection::deferDropCollection(
    std::function<bool(LogicalCollection&)> const& /*callback*/
) {
  // nothing to do here
}

}  // namespace arangodb
