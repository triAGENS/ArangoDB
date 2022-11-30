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
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "VocBase/Identifiers/LocalDocumentId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/voc-types.h"

#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>

namespace arangodb {

ClusterCollection::ClusterCollection(LogicalCollection& collection,
                                     ClusterEngineType engineType,
                                     velocypack::Slice info)
    : PhysicalCollection(collection, info),
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

ClusterCollection::ClusterCollection(LogicalCollection& collection,
                                     PhysicalCollection const* physical)
    : PhysicalCollection(collection, VPackSlice::emptyObjectSlice()),
      _engineType(static_cast<ClusterCollection const*>(physical)->_engineType),
      _info(static_cast<ClusterCollection const*>(physical)->_info),
      _selectivityEstimates(collection) {}

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

std::string const& ClusterCollection::path() const {
  return StaticStrings::Empty;  // we do not have any path
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
  RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
  for (auto& idx : _indexes) {
    // note: we have to exclude inverted indexes here,
    // as they are a different class type (no relationship to
    // ClusterIndex).
    if (idx->type() != Index::TRI_IDX_TYPE_INVERTED_INDEX) {
      TRI_ASSERT(dynamic_cast<ClusterIndex*>(idx.get()) != nullptr);
      static_cast<ClusterIndex*>(idx.get())->updateProperties(_info.slice());
    }
  }

  // nothing else to do
  return {};
}

PhysicalCollection* ClusterCollection::clone(LogicalCollection& logical) const {
  return new ClusterCollection(logical, this);
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

/// @brief closes an open collection
ErrorCode ClusterCollection::close() {
  RECURSIVE_READ_LOCKER(_indexesLock, _indexesLockWriteOwner);
  for (auto it : _indexes) {
    it->unload();
  }

  return TRI_ERROR_NO_ERROR;
}

RevisionId ClusterCollection::revision(transaction::Methods* trx) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

uint64_t ClusterCollection::numberDocuments(transaction::Methods* trx) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

void ClusterCollection::prepareIndexes(velocypack::Slice indexesSlice) {
  RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
  TRI_ASSERT(indexesSlice.isArray());

  StorageEngine& engine = _logicalCollection.vocbase()
                              .server()
                              .getFeature<EngineSelectorFeature>()
                              .engine();
  std::vector<std::shared_ptr<Index>> indexes;

  if (indexesSlice.length() == 0 && _indexes.empty()) {
    engine.indexFactory().fillSystemIndexes(_logicalCollection, indexes);

  } else {
    engine.indexFactory().prepareIndexes(_logicalCollection, indexesSlice,
                                         indexes);
  }

  for (std::shared_ptr<Index>& idx : indexes) {
    addIndex(std::move(idx));
  }

  auto it = _indexes.cbegin();
  if ((*it)->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX ||
      (_logicalCollection.type() == TRI_COL_TYPE_EDGE &&
       ((*++it)->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX ||
        (_indexes.size() >= 3 &&
         _engineType == ClusterEngineType::RocksDBEngine &&
         (*++it)->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX)))) {
    std::string msg = "got invalid indexes for collection '" +
                      _logicalCollection.name() + "'";

    LOG_TOPIC("f71d2", ERR, arangodb::Logger::FIXME) << msg;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    for (auto it : _indexes) {
      LOG_TOPIC("f83f5", ERR, arangodb::Logger::FIXME) << "- " << it->context();
    }
#endif
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
  }

  TRI_ASSERT(!_indexes.empty());
}

Result ClusterCollection::updateIndex(IndexId iid, VPackSlice body, VPackBuilder&) {



  return {TRI_ERROR_NOT_IMPLEMENTED};
}

std::shared_ptr<Index> ClusterCollection::createIndex(velocypack::Slice info,
                                                      bool restore,
                                                      bool& created) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());

  // prevent concurrent dropping
  WRITE_LOCKER(guard, _exclusiveLock);

  RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
  std::shared_ptr<Index> idx = lookupIndex(info);
  if (idx) {
    created = false;
    // We already have this index.
    return idx;
  }

  StorageEngine& engine = _logicalCollection.vocbase()
                              .server()
                              .getFeature<EngineSelectorFeature>()
                              .engine();

  // We are sure that we do not have an index of this type.
  // We also hold the lock. Create it
  idx = engine.indexFactory().prepareIndexFromSlice(info, true,
                                                    _logicalCollection, false);
  TRI_ASSERT(idx != nullptr);

  // In the coordinator case we do not fill the index
  // We only inform the others.
  addIndex(idx);
  created = true;
  return idx;
}

/// @brief Drop an index with the given iid.
bool ClusterCollection::dropIndex(IndexId iid) {
  // usually always called when _exclusiveLock is held
  if (iid.empty() || iid.isPrimary()) {
    return true;
  }

  RECURSIVE_WRITE_LOCKER(_indexesLock, _indexesLockWriteOwner);
  for (auto it : _indexes) {
    if (iid == it->id()) {
      _indexes.erase(it);
      events::DropIndex(_logicalCollection.vocbase().name(),
                        _logicalCollection.name(), std::to_string(iid.id()),
                        TRI_ERROR_NO_ERROR);
      return true;
    }
  }

  // We tried to remove an index that does not exist
  events::DropIndex(_logicalCollection.vocbase().name(),
                    _logicalCollection.name(), std::to_string(iid.id()),
                    TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
  return false;
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
  return Result(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::lookupKey(
    transaction::Methods* /*trx*/, std::string_view /*key*/,
    std::pair<LocalDocumentId, RevisionId>& /*result*/, ReadOwnWrites) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::read(transaction::Methods* /*trx*/,
                               std::string_view /*key*/,
                               IndexIterator::DocumentCallback const& /*cb*/,
                               ReadOwnWrites /*readOwnWrites*/) const {
  return Result(TRI_ERROR_NOT_IMPLEMENTED);
}

// read using a token!
Result ClusterCollection::read(transaction::Methods* /*trx*/,
                               LocalDocumentId const& /*documentId*/,
                               IndexIterator::DocumentCallback const& /*cb*/,
                               ReadOwnWrites /*readOwnWrites*/) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::lookupDocument(
    transaction::Methods& /*trx*/, LocalDocumentId /*documentId*/,
    velocypack::Builder& /*builder*/, bool /*readCache*/, bool /*fillCache*/,
    ReadOwnWrites /*readOwnWrites*/) const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::insert(transaction::Methods&, RevisionId,
                                 velocypack::Slice, OperationOptions const&) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::update(transaction::Methods&, LocalDocumentId,
                                 RevisionId, velocypack::Slice, RevisionId,
                                 velocypack::Slice, OperationOptions const&) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::replace(transaction::Methods&, LocalDocumentId,
                                  RevisionId, velocypack::Slice, RevisionId,
                                  velocypack::Slice, OperationOptions const&) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

Result ClusterCollection::remove(transaction::Methods&, LocalDocumentId,
                                 RevisionId, velocypack::Slice,
                                 OperationOptions const&) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

void ClusterCollection::deferDropCollection(
    std::function<bool(LogicalCollection&)> const& /*callback*/
) {
  // nothing to do here
}

void ClusterCollection::addIndex(std::shared_ptr<Index> idx) {
  // LOCKED from the outside
  auto const id = idx->id();
  for (auto const& it : _indexes) {
    if (it->id() == id) {
      // already have this particular index. do not add it again
      return;
    }
  }
  _indexes.emplace(idx);
}

}  // namespace arangodb
