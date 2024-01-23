////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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

#include "RocksDBPrimaryIndex.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/IndexStreamIterator.h"
#include "Basics/Exceptions.h"
#include "Basics/ResourceUsage.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Cache/BinaryKeyHasher.h"
#include "Cache/CachedValue.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/TransactionalCache.h"
#include "Cluster/ServerState.h"
#ifndef ARANGODB_ENABLE_MAINTAINER_MODE
#include "CrashHandler/CrashHandler.h"
#endif
#include "Indexes/SortedIndexAttributeMatcher.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBKeyBounds.h"
#include "RocksDBEngine/RocksDBTransactionMethods.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "RocksDBEngine/RocksDBValue.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/OperationOptions.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"

#include "RocksDBEngine/RocksDBPrefixExtractor.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/VocBase/VirtualClusterSmartEdgeCollection.h"
#endif

#include <absl/strings/str_cat.h>

#include <rocksdb/iterator.h>
#include <rocksdb/utilities/transaction.h>

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Slice.h>
#include <velocypack/String.h>

using namespace arangodb;
using namespace arangodb::basics;

namespace {
using PrimaryIndexCacheType = cache::TransactionalCache<cache::BinaryKeyHasher>;
}  // namespace

// ================ Primary Index Iterators ================

namespace arangodb {

class RocksDBPrimaryIndexEqIterator final : public IndexIterator {
 public:
  RocksDBPrimaryIndexEqIterator(ResourceMonitor& /*monitor*/,
                                LogicalCollection* collection,
                                transaction::Methods* trx,
                                RocksDBPrimaryIndex* index, VPackBuilder key,
                                bool lookupByIdAttribute,
                                ReadOwnWrites readOwnWrites)
      : IndexIterator(collection, trx, readOwnWrites),
        _index(index),
        _key(std::move(key)),
        _isId(lookupByIdAttribute),
        _withCache(_index->useCache() != nullptr),
        _done(false) {
    TRI_ASSERT(_key.slice().isString());

    // no need to track memory usage here, as this iterator is not
    // employing a RocksDB iterator in the background, but only performs
    // simple RocksDB Get operations. The lookup value is also only a
    // single key, so tracking memory usage would not be justified
  }

  std::string_view typeName() const noexcept final {
    return "primary-index-eq-iterator";
  }

  /// @brief index supports rearming
  bool canRearm() const override { return true; }

  /// @brief rearm the index iterator
  bool rearmImpl(aql::AstNode const* node, aql::Variable const* variable,
                 IndexIteratorOptions const& /*opts*/) override {
    TRI_ASSERT(node != nullptr);
    TRI_ASSERT(node->type == aql::NODE_TYPE_OPERATOR_NARY_AND);
    TRI_ASSERT(node->numMembers() == 1);
    AttributeAccessParts aap(node->getMember(0), variable);
    TRI_ASSERT(aap.opType == aql::NODE_TYPE_OPERATOR_BINARY_EQ);

    // handle the sole element
    _key.clear();
    _index->handleValNode(_trx, _key, aap.value, _isId);

    TRI_IF_FAILURE("PrimaryIndex::noIterator") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    return !_key.isEmpty();
  }

  bool nextImpl(LocalDocumentIdCallback const& cb, uint64_t limit) override {
    if (limit == 0 || _done) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      return false;
    }

    _done = true;

    bool foundInCache = false;
    LocalDocumentId documentId = _index->lookupKey(
        _trx, _key.slice().stringView(), canReadOwnWrites(), foundInCache);
    if (documentId.isSet()) {
      cb(documentId);
    }
    if (_withCache) {
      incrCacheStats(foundInCache);
    }
    return false;
  }

  /// @brief extracts just _key. not supported for use with _id
  bool nextCoveringImpl(CoveringCallback const& cb, uint64_t limit) override {
    if (limit == 0 || _done) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      return false;
    }

    _done = true;

    bool foundInCache = false;
    LocalDocumentId documentId = _index->lookupKey(
        _trx, _key.slice().stringView(), canReadOwnWrites(), foundInCache);
    if (documentId.isSet()) {
      auto data = SliceCoveringData(_key.slice());
      cb(documentId, data);
    }
    if (_withCache) {
      incrCacheStats(foundInCache);
    }
    return false;
  }

  void resetImpl() final { _done = false; }

 private:
  RocksDBPrimaryIndex* _index;
  VPackBuilder _key;
  bool const _isId;
  bool const _withCache;
  bool _done;
};

class RocksDBPrimaryIndexInIterator final : public IndexIterator {
 public:
  RocksDBPrimaryIndexInIterator(ResourceMonitor& monitor,
                                LogicalCollection* collection,
                                transaction::Methods* trx,
                                RocksDBPrimaryIndex* index, VPackBuilder keys,
                                bool lookupByIdAttribute)
      : IndexIterator(collection, trx,
                      ReadOwnWrites::no),  // "in"-checks never need to observe
                                           // own writes.
        _resourceMonitor(monitor),
        _index(index),
        _keys(std::move(keys)),
        _iterator(_keys.slice()),
        _memoryUsage(0),
        _isId(lookupByIdAttribute),
        _withCache(_index->useCache() != nullptr) {
    TRI_ASSERT(_keys.slice().isArray());

    ResourceUsageScope scope(_resourceMonitor, _keys.size());
    // now we are responsible for tracking memory usage
    _memoryUsage += scope.trackedAndSteal();
  }

  ~RocksDBPrimaryIndexInIterator() override {
    _resourceMonitor.decreaseMemoryUsage(_memoryUsage);
  }

  std::string_view typeName() const noexcept final {
    return "primary-index-in-iterator";
  }

  /// @brief index supports rearming
  bool canRearm() const override { return true; }

  /// @brief rearm the index iterator
  bool rearmImpl(aql::AstNode const* node, aql::Variable const* variable,
                 IndexIteratorOptions const& opts) override {
    TRI_ASSERT(node != nullptr);
    TRI_ASSERT(node->type == aql::NODE_TYPE_OPERATOR_NARY_AND);
    TRI_ASSERT(node->numMembers() == 1);
    AttributeAccessParts aap(node->getMember(0), variable);
    TRI_ASSERT(aap.opType == aql::NODE_TYPE_OPERATOR_BINARY_IN);

    if (aap.value->isArray()) {
      size_t oldMemoryUsage = _keys.size();
      TRI_ASSERT(_memoryUsage >= oldMemoryUsage);
      _resourceMonitor.decreaseMemoryUsage(oldMemoryUsage);
      _memoryUsage -= oldMemoryUsage;

      _keys.clear();
      _index->fillInLookupValues(_trx, _keys, aap.value, opts.ascending, _isId);
      _iterator = VPackArrayIterator(_keys.slice());

      size_t newMemoryUsage = _keys.size();
      _resourceMonitor.increaseMemoryUsage(newMemoryUsage);
      _memoryUsage += newMemoryUsage;
      return true;
    }

    return false;
  }

  bool nextImpl(LocalDocumentIdCallback const& cb, uint64_t limit) override {
    if (limit == 0 || !_iterator.valid()) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      return false;
    }

    while (limit > 0) {
      // This is an in-iterator, and "in"-checks never need to observe own
      // writes.
      bool foundInCache = false;
      LocalDocumentId documentId = _index->lookupKey(
          _trx, (*_iterator).stringView(), ReadOwnWrites::no, foundInCache);
      if (documentId.isSet()) {
        cb(documentId);
        --limit;
      }
      if (_withCache) {
        incrCacheStats(foundInCache);
      }

      _iterator.next();
      if (!_iterator.valid()) {
        return false;
      }
    }
    return true;
  }

  bool nextCoveringImpl(CoveringCallback const& cb, uint64_t limit) override {
    if (limit == 0 || !_iterator.valid()) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      return false;
    }

    while (limit > 0) {
      // This is an in-iterator, and "in"-checks never need to observe own
      // writes.
      bool foundInCache = false;
      LocalDocumentId documentId = _index->lookupKey(
          _trx, (*_iterator).stringView(), ReadOwnWrites::no, foundInCache);
      if (documentId.isSet()) {
        auto data = SliceCoveringData(*_iterator);
        cb(documentId, data);
        --limit;
      }
      if (_withCache) {
        incrCacheStats(foundInCache);
      }

      _iterator.next();
      if (!_iterator.valid()) {
        return false;
      }
    }
    return true;
  }

  void resetImpl() final { _iterator.reset(); }

 private:
  ResourceMonitor& _resourceMonitor;
  RocksDBPrimaryIndex* _index;
  VPackBuilder _keys;
  velocypack::ArrayIterator _iterator;
  size_t _memoryUsage;
  bool const _isId;
  bool const _withCache;
};

template<bool reverse, bool mustCheckBounds>
class RocksDBPrimaryIndexRangeIterator final : public IndexIterator {
 public:
  RocksDBPrimaryIndexRangeIterator(ResourceMonitor& monitor,
                                   LogicalCollection* collection,
                                   transaction::Methods* trx,
                                   RocksDBPrimaryIndex const* index,
                                   RocksDBKeyBounds&& bounds,
                                   ReadOwnWrites readOwnWrites)
      : IndexIterator(collection, trx, readOwnWrites),
        _resourceMonitor(monitor),
        _index(index),
        _cmp(index->comparator()),
        _mustSeek(true),
        _bounds(std::move(bounds)),
        _rangeBound(reverse ? _bounds.start() : _bounds.end()),
        _memoryUsage(0) {
    TRI_ASSERT(index->columnFamily() ==
               RocksDBColumnFamilyManager::get(
                   RocksDBColumnFamilyManager::Family::PrimaryIndex));
  }

  ~RocksDBPrimaryIndexRangeIterator() override {
    _resourceMonitor.decreaseMemoryUsage(_memoryUsage);
  }

  std::string_view typeName() const noexcept final {
    return "primary-index-range-iterator";
  }

  /// @brief index does not support rearming
  bool canRearm() const override { return false; }

  /// @brief Get the next limit many elements in the index
  bool nextImpl(LocalDocumentIdCallback const& cb, uint64_t limit) override {
    ensureIterator();
    TRI_ASSERT(_trx->state()->isRunning());
    TRI_ASSERT(_iterator != nullptr);

    if (!_iterator->Valid() || outOfRange() || ADB_UNLIKELY(limit == 0)) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      // validate that Iterator is in a good shape and hasn't failed
      rocksutils::checkIteratorStatus(*_iterator);
      return false;
    }

    TRI_ASSERT(limit > 0);

    do {
      TRI_ASSERT(_index->objectId() == RocksDBKey::objectId(_iterator->key()));

      cb(RocksDBValue::documentId(_iterator->value()));

      if constexpr (reverse) {
        _iterator->Prev();
      } else {
        _iterator->Next();
      }

      if (ADB_UNLIKELY(!_iterator->Valid())) {
        // validate that Iterator is in a good shape and hasn't failed
        rocksutils::checkIteratorStatus(*_iterator);
        return false;
      } else if (outOfRange()) {
        return false;
      }

      --limit;
      if (limit == 0) {
        return true;
      }
    } while (true);
  }

  bool nextCoveringImpl(CoveringCallback const& cb, uint64_t limit) override {
    ensureIterator();
    TRI_ASSERT(_trx->state()->isRunning());
    TRI_ASSERT(_iterator != nullptr);

    if (!_iterator->Valid() || outOfRange() || ADB_UNLIKELY(limit == 0)) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      // validate that Iterator is in a good shape and hasn't failed
      rocksutils::checkIteratorStatus(*_iterator);
      return false;
    }

    TRI_ASSERT(limit > 0);

    transaction::BuilderLeaser builder(transaction());

    do {
      TRI_ASSERT(_index->objectId() == RocksDBKey::objectId(_iterator->key()));
      LocalDocumentId documentId = RocksDBValue::documentId(_iterator->value());
      std::string_view key = RocksDBKey::primaryKey(_iterator->key());

      builder->clear();
      builder->add(VPackValue(key));
      auto data = SliceCoveringData(builder->slice());
      cb(documentId, data);

      if constexpr (reverse) {
        _iterator->Prev();
      } else {
        _iterator->Next();
      }

      if (ADB_UNLIKELY(!_iterator->Valid())) {
        // validate that Iterator is in a good shape and hasn't failed
        rocksutils::checkIteratorStatus(*_iterator);
        return false;
      } else if (outOfRange()) {
        return false;
      }

      --limit;
      if (limit == 0) {
        return true;
      }
    } while (true);
  }

  void skipImpl(uint64_t count, uint64_t& skipped) override {
    ensureIterator();
    TRI_ASSERT(_trx->state()->isRunning());
    TRI_ASSERT(_iterator != nullptr);

    if (_iterator->Valid() && !outOfRange() && count > 0) {
      do {
        TRI_ASSERT(_index->objectId() ==
                   RocksDBKey::objectId(_iterator->key()));

        --count;
        ++skipped;
        if constexpr (reverse) {
          _iterator->Prev();
        } else {
          _iterator->Next();
        }

        if (!_iterator->Valid() || outOfRange()) {
          break;
        }

        if (count == 0) {
          return;
        }
      } while (true);
    }

    // validate that Iterator is in a good shape and hasn't failed
    rocksutils::checkIteratorStatus(*_iterator);
  }

  /// @brief Reset the cursor
  void resetImpl() final {
    TRI_ASSERT(_trx->state()->isRunning());
    _mustSeek = true;
  }

 private:
  void ensureIterator() {
    if (_iterator == nullptr) {
      // the RocksDB iterator _iterator is only built once during the
      // lifetime of the RocksPrimaryIndexRangeIterator. so it is ok
      // to track its expected memory usage here and only count it down
      // when we destroy the RocksDBPrimaryIndexRangeIterator object
      ResourceUsageScope scope(_resourceMonitor, expectedIteratorMemoryUsage);

      auto state = RocksDBTransactionState::toState(_trx);
      RocksDBTransactionMethods* mthds =
          state->rocksdbMethods(_collection->id());
      _iterator = mthds->NewIterator(
          _index->columnFamily(), [&](rocksdb::ReadOptions& options) {
            TRI_ASSERT(options.prefix_same_as_start);
            // note: iterate_lower_bound/iterate_upper_bound should only be
            // set if the iterator is not supposed to check the bounds
            // for every operation.
            // when the iterator is a db snapshot-based iterator, it is ok
            // to set iterate_lower_bound/iterate_upper_bound, because this
            // is well supported by RocksDB.
            // if the iterator is a multi-level iterator that merges data from
            // the db snapshot with data from an ongoing in-memory transaction
            // (contained in a WriteBatchWithIndex, WBWI), then RocksDB does
            // not properly support the bounds checking using
            // iterate_lower_bound/ iterate_upper_bound. in this case we must
            // avoid setting the bounds here and rely on our own bounds checking
            // using the comparator. at least one underlying issue was fixed in
            // RocksDB in version 8.8.0 via
            // https://github.com/facebook/rocksdb/pull/11680. we can revisit
            // the issue once we have upgraded to RocksDB >= 8.8.0.
            if constexpr (!mustCheckBounds) {
              // we need to have a pointer to a slice for the upper bound
              // so we need to assign the slice to an instance variable here
              if constexpr (reverse) {
                options.iterate_lower_bound = &_rangeBound;
              } else {
                options.iterate_upper_bound = &_rangeBound;
              }
            }
          });
      TRI_ASSERT(_mustSeek);

      // now we are responsible for tracking the memory usage
      _memoryUsage += scope.trackedAndSteal();
    }

    TRI_ASSERT(_iterator != nullptr);
    if (_mustSeek) {
      if constexpr (reverse) {
        _iterator->SeekForPrev(_bounds.end());
      } else {
        _iterator->Seek(_bounds.start());
      }
      _mustSeek = false;
    }
    TRI_ASSERT(!_mustSeek);
  }

  inline bool outOfRange() const {
    if constexpr (mustCheckBounds) {
      // we can effectively disable the out-of-range checks for read-only
      // transactions, as our Iterator is a snapshot-based iterator with a
      // configured iterate_upper_bound/iterate_lower_bound value.
      // this makes RocksDB filter out non-matching keys automatically.
      // however, for a write transaction our Iterator is a rocksdb
      // BaseDeltaIterator, which will merge the values from a snapshot iterator
      // and the changes in the current transaction. here rocksdb will only
      // apply the bounds checks for the base iterator (from the snapshot), but
      // not for the delta iterator (from the current transaction), so we still
      // have to carry out the checks ourselves.
      int res = _cmp->Compare(_iterator->key(), _rangeBound);
      if constexpr (reverse) {
        return res < 0;
      } else {
        return res > 0;
      }
    } else {
      return false;
    }
  }

  // expected number of bytes that a RocksDB iterator will use.
  // this is a guess and does not need to be fully accurate.
  static constexpr size_t expectedIteratorMemoryUsage = 8192;

  ResourceMonitor& _resourceMonitor;
  RocksDBPrimaryIndex const* _index;
  rocksdb::Comparator const* _cmp;
  std::unique_ptr<rocksdb::Iterator> _iterator;
  bool _mustSeek;
  RocksDBKeyBounds const _bounds;
  // used for iterate_upper_bound iterate_lower_bound
  rocksdb::Slice _rangeBound;
  size_t _memoryUsage;
};

}  // namespace arangodb

// ================ PrimaryIndex ================

RocksDBPrimaryIndex::RocksDBPrimaryIndex(LogicalCollection& collection,
                                         velocypack::Slice info)
    : RocksDBIndex(
          IndexId::primary(), collection, StaticStrings::IndexNamePrimary,
          std::vector<std::vector<basics::AttributeName>>(
              {{basics::AttributeName(StaticStrings::KeyString, false)}}),
          true, false,
          RocksDBColumnFamilyManager::get(
              RocksDBColumnFamilyManager::Family::PrimaryIndex),
          basics::VelocyPackHelper::stringUInt64(info, StaticStrings::ObjectId),
          /*useCache*/
          static_cast<RocksDBCollection*>(collection.getPhysical())
              ->cacheEnabled(),
          /*cacheManager*/
          collection.vocbase()
              .server()
              .getFeature<CacheManagerFeature>()
              .manager(),
          /*engine*/
          collection.vocbase()
              .server()
              .getFeature<EngineSelectorFeature>()
              .engine<RocksDBEngine>()),
      _coveredFields({{AttributeName(StaticStrings::KeyString, false)},
                      {AttributeName(StaticStrings::IdString, false)}}) {
  TRI_ASSERT(_cf == RocksDBColumnFamilyManager::get(
                        RocksDBColumnFamilyManager::Family::PrimaryIndex));
  TRI_ASSERT(objectId() != 0);

  if (_cacheEnabled) {
    setupCache();
  }
}

RocksDBPrimaryIndex::~RocksDBPrimaryIndex() = default;

std::vector<std::vector<basics::AttributeName>> const&
RocksDBPrimaryIndex::coveredFields() const {
  return _coveredFields;
}

void RocksDBPrimaryIndex::load() {
  RocksDBIndex::load();
  if (auto cache = useCache()) {
    // FIXME: make the factor configurable
    RocksDBCollection* rdb =
        static_cast<RocksDBCollection*>(_collection.getPhysical());
    uint64_t numDocs = rdb->meta().numberDocuments();

    if (numDocs > 0) {
      cache->sizeHint(static_cast<uint64_t>(0.3 * numDocs));
    }
  }
}

/// @brief return a VelocyPack representation of the index
void RocksDBPrimaryIndex::toVelocyPack(
    VPackBuilder& builder, std::underlying_type<Serialize>::type flags) const {
  builder.openObject();
  RocksDBIndex::toVelocyPack(builder, flags);
  builder.close();
}

LocalDocumentId RocksDBPrimaryIndex::lookupKey(transaction::Methods* trx,
                                               std::string_view keyRef,
                                               ReadOwnWrites readOwnWrites,
                                               bool& foundInCache) const {
  RocksDBKeyLeaser key(trx);
  key->constructPrimaryIndexValue(objectId(), keyRef);

  foundInCache = false;
  bool lockTimeout = false;
  auto cache = useCache();
  if (cache != nullptr) {
    // check cache first for fast path
    auto f = cache->find(key->string().data(),
                         static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      foundInCache = true;
      rocksdb::Slice s(reinterpret_cast<char const*>(f.value()->value()),
                       f.value()->valueSize());
      return RocksDBValue::documentId(s);
    } else if (f.result() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  RocksDBMethods* mthds =
      RocksDBTransactionState::toMethods(trx, _collection.id());
  rocksdb::PinnableSlice val;
  rocksdb::Status s = mthds->Get(_cf, key->string(), &val, readOwnWrites);
  if (!s.ok()) {
    return LocalDocumentId();
  }

  if (cache != nullptr && !lockTimeout) {
    // write entry back to cache
    cache::Cache::SimpleInserter<PrimaryIndexCacheType>{
        static_cast<PrimaryIndexCacheType&>(*cache), key->string().data(),
        static_cast<uint32_t>(key->string().size()), val.data(),
        static_cast<uint64_t>(val.size())};
  }

  return RocksDBValue::documentId(val);
}

/// @brief reads a revision id from the primary index
/// if the document does not exist, this function will return false
/// if the document exists, the function will return true
/// the revision id will only be non-zero if the primary index
/// value contains the document's revision id. note that this is not
/// the case for older collections
/// in this case the caller must fetch the revision id from the actual
/// document
Result RocksDBPrimaryIndex::lookupRevision(transaction::Methods* trx,
                                           std::string_view keyRef,
                                           LocalDocumentId& documentId,
                                           RevisionId& revisionId,
                                           ReadOwnWrites readOwnWrites,
                                           bool lockForUpdate) const {
  documentId = LocalDocumentId::none();
  revisionId = RevisionId::none();

  RocksDBKeyLeaser key(trx);
  key->constructPrimaryIndexValue(objectId(), keyRef);

  // acquire rocksdb transaction
  RocksDBMethods* mthds =
      RocksDBTransactionState::toMethods(trx, _collection.id());
  rocksdb::PinnableSlice val;
  rocksdb::Status s;
  if (lockForUpdate) {
    TRI_ASSERT(readOwnWrites == ReadOwnWrites::yes);
    s = mthds->GetForUpdate(_cf, key->string(), &val);
  } else {
    s = mthds->Get(_cf, key->string(), &val, readOwnWrites);
  }
  if (!s.ok()) {
    return rocksutils::convertStatus(s, rocksutils::StatusHint::document);
  }

  documentId = RocksDBValue::documentId(val);

  // this call will populate revisionId if the revision id value is
  // stored in the primary index
  revisionId = RocksDBValue::revisionId(val);
  return Result{};
}

Result RocksDBPrimaryIndex::checkInsert(transaction::Methods& trx,
                                        RocksDBMethods* mthd,
                                        LocalDocumentId /*documentId*/,
                                        velocypack::Slice slice,
                                        OperationOptions const& options) {
  // this is already handled earlier - nothing to do here!
  return {};
}

Result RocksDBPrimaryIndex::checkReplace(transaction::Methods& trx,
                                         RocksDBMethods* mthd,
                                         LocalDocumentId /*documentId*/,
                                         velocypack::Slice slice,
                                         OperationOptions const& options) {
  // this is already handled earlier - nothing to do here!
  return {};
}

Result RocksDBPrimaryIndex::insert(transaction::Methods& trx,
                                   RocksDBMethods* mthd,
                                   LocalDocumentId documentId,
                                   velocypack::Slice slice,
                                   OperationOptions const& options,
                                   bool performChecks) {
  VPackSlice keySlice;
  RevisionId revision;
  transaction::helpers::extractKeyAndRevFromDocument(slice, keySlice, revision);
  TRI_ASSERT(keySlice.isString());

  RocksDBKeyLeaser key(&trx);
  key->constructPrimaryIndexValue(objectId(), keySlice.stringView());

  // we do not need to perform any additional checks here since the document key
  // is already locked at the beginning of the insert operation

  if (trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    // invalidate new index cache entry to avoid caching without committing
    // first
    invalidateCacheEntry(key->string().data(),
                         static_cast<uint32_t>(key->string().size()));
  }

  TRI_ASSERT(revision.isSet());
  auto value = RocksDBValue::PrimaryIndexValue(documentId, revision);
  rocksdb::Status s =
      mthd->Put(_cf, key.ref(), value.string(), /*assume_tracked*/ true);
  if (!s.ok()) {
    Result res = rocksutils::convertStatus(s, rocksutils::index);
    addErrorMsg(res, keySlice.stringView());
    return res;
  }
  return {};
}

Result RocksDBPrimaryIndex::update(
    transaction::Methods& trx, RocksDBMethods* mthd,
    LocalDocumentId /*oldDocumentId*/, velocypack::Slice oldDoc,
    LocalDocumentId newDocumentId, velocypack::Slice newDoc,
    OperationOptions const& /*options*/, bool /*performChecks*/) {
  VPackSlice keySlice = transaction::helpers::extractKeyFromDocument(oldDoc);
  TRI_ASSERT(keySlice.binaryEquals(oldDoc.get(StaticStrings::KeyString)));

  Result res;
  if (oldDoc.get(StaticStrings::KeyString).stringView() !=
      newDoc.get(StaticStrings::KeyString).stringView()) {
    res.reset(
        TRI_ERROR_INTERNAL,
        absl::StrCat("invalid primary index update in '",
                     _collection.vocbase().name(), "/", _collection.name()));
    res.withError([&oldDoc, &newDoc](result::Error& err) {
      err.appendErrorMessage("; old key: ");
      err.appendErrorMessage(oldDoc.get(StaticStrings::KeyString).copyString());
      err.appendErrorMessage("; new key: ");
      err.appendErrorMessage(newDoc.get(StaticStrings::KeyString).copyString());
    });
#ifndef ARANGODB_ENABLE_MAINTAINER_MODE
    LOG_TOPIC("f3b56", ERR, Logger::ENGINES) << res.errorMessage();
    CrashHandler::logBacktrace();
#endif
    TRI_ASSERT(false) << res.errorMessage();
    return res;
  }

  RocksDBKeyLeaser key(&trx);

  key->constructPrimaryIndexValue(objectId(), keySlice.stringView());

  RevisionId revision = transaction::helpers::extractRevFromDocument(newDoc);
  auto value = RocksDBValue::PrimaryIndexValue(newDocumentId, revision);

  // invalidate new index cache entry to avoid caching without committing first
  invalidateCacheEntry(key->string().data(),
                       static_cast<uint32_t>(key->string().size()));

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  {
    rocksdb::Status s = mthd->GetForUpdate(_cf, key->string(), nullptr);
    if (s.IsNotFound()) {
      // the key must have existed before in the primary index
      res.reset(TRI_ERROR_ARANGO_CONFLICT,
                "inconsistency with non-existing primary key detected");
      res.withError([&oldDoc, &newDoc](result::Error& err) {
        err.appendErrorMessage("; old key: ");
        err.appendErrorMessage(
            oldDoc.get(StaticStrings::KeyString).copyString());
        err.appendErrorMessage("; new key: ");
        err.appendErrorMessage(
            newDoc.get(StaticStrings::KeyString).copyString());
      });
      TRI_ASSERT(false) << "primary index update: " << res.errorMessage();
      return res;
    }
  }
#endif

  rocksdb::Status s =
      mthd->Put(_cf, key.ref(), value.string(), /*assume_tracked*/ false);
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::index));
    addErrorMsg(res, keySlice.stringView());
  }
  return res;
}

Result RocksDBPrimaryIndex::remove(transaction::Methods& trx,
                                   RocksDBMethods* /*mthd*/,
                                   LocalDocumentId /*documentId*/,
                                   velocypack::Slice slice,
                                   OperationOptions const& /*options*/) {
  Result res;

  // TODO: deal with matching revisions?
  VPackSlice keySlice = transaction::helpers::extractKeyFromDocument(slice);
  TRI_ASSERT(keySlice.isString());
  RocksDBKeyLeaser key(&trx);
  key->constructPrimaryIndexValue(objectId(), keySlice.stringView());

  invalidateCacheEntry(key->string().data(),
                       static_cast<uint32_t>(key->string().size()));

  // acquire rocksdb transaction
  auto* mthds = RocksDBTransactionState::toMethods(&trx, _collection.id());
  rocksdb::Status s = mthds->Delete(_cf, key.ref());
  if (!s.ok()) {
    res.reset(rocksutils::convertStatus(s, rocksutils::index));
    addErrorMsg(res, keySlice.stringView());
  }
  return res;
}

/// @brief checks whether the index supports the condition
Index::FilterCosts RocksDBPrimaryIndex::supportsFilterCondition(
    transaction::Methods& /*trx*/,
    std::vector<std::shared_ptr<Index>> const& allIndexes,
    aql::AstNode const* node, aql::Variable const* reference,
    size_t itemsInIndex) const {
  return SortedIndexAttributeMatcher::supportsFilterCondition(
      allIndexes, this, node, reference, itemsInIndex);
}

Index::SortCosts RocksDBPrimaryIndex::supportsSortCondition(
    aql::SortCondition const* sortCondition, aql::Variable const* reference,
    size_t itemsInIndex) const {
  return SortedIndexAttributeMatcher::supportsSortCondition(
      this, sortCondition, reference, itemsInIndex);
}

/// @brief creates an IndexIterator for the given Condition
std::unique_ptr<IndexIterator> RocksDBPrimaryIndex::iteratorForCondition(
    ResourceMonitor& monitor, transaction::Methods* trx,
    aql::AstNode const* node, aql::Variable const* reference,
    IndexIteratorOptions const& opts, ReadOwnWrites readOwnWrites, int) {
  TRI_ASSERT(!isSorted() || opts.sorted);

  bool mustCheckBounds =
      RocksDBTransactionState::toState(trx)->iteratorMustCheckBounds(
          _collection.id(), readOwnWrites);

  if (node == nullptr) {
    // full range scan
    if (opts.ascending) {
      // forward version
      if (mustCheckBounds) {
        return std::make_unique<RocksDBPrimaryIndexRangeIterator<false, true>>(
            monitor, &_collection /*logical collection*/, trx, this,
            RocksDBKeyBounds::PrimaryIndex(objectId(),
                                           KeyGeneratorHelper::lowestKey,
                                           KeyGeneratorHelper::highestKey),
            readOwnWrites);
      }
      return std::make_unique<RocksDBPrimaryIndexRangeIterator<false, false>>(
          monitor, &_collection /*logical collection*/, trx, this,
          RocksDBKeyBounds::PrimaryIndex(objectId(),
                                         KeyGeneratorHelper::lowestKey,
                                         KeyGeneratorHelper::highestKey),
          readOwnWrites);
    }
    // reverse version
    if (mustCheckBounds) {
      return std::make_unique<RocksDBPrimaryIndexRangeIterator<true, true>>(
          monitor, &_collection /*logical collection*/, trx, this,
          RocksDBKeyBounds::PrimaryIndex(objectId(),
                                         KeyGeneratorHelper::lowestKey,
                                         KeyGeneratorHelper::highestKey),
          readOwnWrites);
    }
    return std::make_unique<RocksDBPrimaryIndexRangeIterator<true, false>>(
        monitor, &_collection /*logical collection*/, trx, this,
        RocksDBKeyBounds::PrimaryIndex(objectId(),
                                       KeyGeneratorHelper::lowestKey,
                                       KeyGeneratorHelper::highestKey),
        readOwnWrites);
  }

  TRI_ASSERT(node != nullptr);
  TRI_ASSERT(node->type == aql::NODE_TYPE_OPERATOR_NARY_AND);

  size_t const n = node->numMembers();
  TRI_ASSERT(n >= 1);

  if (n == 1) {
    AttributeAccessParts aap(node->getMember(0), reference);

    if (aap.opType == aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
      // a.b == value
      return createEqIterator(monitor, trx, aap.attribute, aap.value,
                              readOwnWrites);
    }
    if (aap.opType == aql::NODE_TYPE_OPERATOR_BINARY_IN &&
        aap.value->isArray()) {
      // "in"-checks never need to observe own writes.
      TRI_ASSERT(readOwnWrites == ReadOwnWrites::no);
      // a.b IN array
      return createInIterator(monitor, trx, aap.attribute, aap.value,
                              opts.ascending);
    }
    // fall-through intentional here
  }

  auto removeCollectionFromString = [this, &trx](bool isId,
                                                 std::string& value) -> int {
    if (isId) {
      char const* key = nullptr;
      size_t outLength = 0;
      std::shared_ptr<LogicalCollection> collection;
      Result res = trx->resolveId(value.data(), value.length(), collection, key,
                                  outLength);

      bool isRunningInCluster = ServerState::instance()->isRunningInCluster();

      if (!res.ok()) {
        // using the name of an unknown collection
        if (isRunningInCluster) {
          // translate from our own shard name to "real" collection name
          return value.compare(
              trx->resolver()->getCollectionName(_collection.id()));
        }
        return value.compare(_collection.name());
      }

      TRI_ASSERT(key);
      TRI_ASSERT(collection);

      if (!isRunningInCluster && collection->id() != _collection.id()) {
        // using the name of a different collection...
        return value.compare(_collection.name());
      } else if (isRunningInCluster &&
                 collection->planId() != _collection.planId()) {
        // using a different collection
        // translate from our own shard name to "real" collection name
        return value.compare(
            trx->resolver()->getCollectionName(_collection.id()));
      }

      // strip collection name prefix
      value = std::string(key, outLength);
    }

    // usage of _key or same collection name
    return 0;
  };

  std::string lower;
  std::string upper;
  bool lowerFound = false;
  bool upperFound = false;

  for (size_t i = 0; i < n; ++i) {
    AttributeAccessParts aap(node->getMemberUnchecked(i), reference);

    auto type = aap.opType;

    if (!(type == aql::NODE_TYPE_OPERATOR_BINARY_LE ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_LT ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_GE ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_GT ||
          type == aql::NODE_TYPE_OPERATOR_BINARY_EQ)) {
      return std::make_unique<EmptyIndexIterator>(&_collection, trx);
    }

    TRI_ASSERT(aap.attribute->type == aql::NODE_TYPE_ATTRIBUTE_ACCESS);
    bool const isId = (aap.attribute->stringEquals(StaticStrings::IdString));

    std::string value;  // empty string == lower bound
    if (aap.value->isStringValue()) {
      value = aap.value->getString();
    } else if (aap.value->isObject() || aap.value->isArray()) {
      // any array or object value is bigger than any potential key
      value = KeyGeneratorHelper::highestKey;
    } else if (aap.value->isNullValue() || aap.value->isBoolValue() ||
               aap.value->isIntValue()) {
      // any null, bool or numeric value is lower than any potential key
      // keep lower bound
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, absl::StrCat("unhandled type for valNode: ",
                                           aap.value->getTypeString()));
    }

    // strip collection name prefix from comparison value
    int const cmpResult = removeCollectionFromString(isId, value);

    if (type == aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
      if (cmpResult != 0) {
        // doc._id == different collection
        return std::make_unique<EmptyIndexIterator>(&_collection, trx);
      }
      if (!upperFound || value < upper) {
        upper = value;
        upperFound = true;
      }
      if (!lowerFound || value < lower) {
        lower = std::move(value);
        lowerFound = true;
      }
    } else if (type == aql::NODE_TYPE_OPERATOR_BINARY_LE ||
               type == aql::NODE_TYPE_OPERATOR_BINARY_LT) {
      // a.b < value
      if (cmpResult > 0) {
        // doc._id < collection with "bigger" name
        upper = KeyGeneratorHelper::highestKey;
      } else if (cmpResult < 0) {
        // doc._id < collection with "lower" name
        return std::make_unique<EmptyIndexIterator>(&_collection, trx);
      } else {
        if (type == aql::NODE_TYPE_OPERATOR_BINARY_LT && !value.empty()) {
          // modify upper bound so that it is not included
          // primary keys are ASCII only, so we don't need to care about UTF-8
          // characters here
          if (value.back() >= static_cast<std::string::value_type>(0x02)) {
            value.back() -= 0x01;
            value.append(KeyGeneratorHelper::highestKey);
          }
        }
        if (!upperFound || value < upper) {
          upper = std::move(value);
        }
      }
      upperFound = true;
    } else if (type == aql::NODE_TYPE_OPERATOR_BINARY_GE ||
               type == aql::NODE_TYPE_OPERATOR_BINARY_GT) {
      // a.b > value
      if (cmpResult < 0) {
        // doc._id > collection with "smaller" name
        lower = KeyGeneratorHelper::lowestKey;
      } else if (cmpResult > 0) {
        // doc._id > collection with "bigger" name
        return std::make_unique<EmptyIndexIterator>(&_collection, trx);
      } else {
        if (type == aql::NODE_TYPE_OPERATOR_BINARY_GE && !value.empty()) {
          // modify lower bound so it is included in the results
          // primary keys are ASCII only, so we don't need to care about UTF-8
          // characters here
          if (value.back() >= static_cast<std::string::value_type>(0x02)) {
            value.back() -= 0x01;
            value.append(KeyGeneratorHelper::highestKey);
          }
        }
        if (!lowerFound || value > lower) {
          lower = std::move(value);
        }
      }
      lowerFound = true;
    }
  }  // for nodes

  // if only one bound is given select the other (lowest or highest) accordingly
  if (upperFound && !lowerFound) {
    lower = KeyGeneratorHelper::lowestKey;
    lowerFound = true;
  } else if (lowerFound && !upperFound) {
    upper = KeyGeneratorHelper::highestKey;
    upperFound = true;
  }

  if (lowerFound && upperFound) {
    if (opts.ascending) {
      // forward version
      if (mustCheckBounds) {
        return std::make_unique<RocksDBPrimaryIndexRangeIterator<false, true>>(
            monitor, &_collection /*logical collection*/, trx, this,
            RocksDBKeyBounds::PrimaryIndex(objectId(), lower, upper),
            readOwnWrites);
      }
      return std::make_unique<RocksDBPrimaryIndexRangeIterator<false, false>>(
          monitor, &_collection /*logical collection*/, trx, this,
          RocksDBKeyBounds::PrimaryIndex(objectId(), lower, upper),
          readOwnWrites);
    }
    // reverse version
    if (mustCheckBounds) {
      return std::make_unique<RocksDBPrimaryIndexRangeIterator<true, true>>(
          monitor, &_collection /*logical collection*/, trx, this,
          RocksDBKeyBounds::PrimaryIndex(objectId(), lower, upper),
          readOwnWrites);
    }
    return std::make_unique<RocksDBPrimaryIndexRangeIterator<true, false>>(
        monitor, &_collection /*logical collection*/, trx, this,
        RocksDBKeyBounds::PrimaryIndex(objectId(), lower, upper),
        readOwnWrites);
  }

  // operator type unsupported or IN used on non-array
  return std::make_unique<EmptyIndexIterator>(&_collection, trx);
}

/// @brief specializes the condition for use with the index
aql::AstNode* RocksDBPrimaryIndex::specializeCondition(
    transaction::Methods& /*trx*/, aql::AstNode* node,
    aql::Variable const* reference) const {
  return SortedIndexAttributeMatcher::specializeCondition(this, node,
                                                          reference);
}

/// @brief create the iterator, for a single attribute, IN operator
std::unique_ptr<IndexIterator> RocksDBPrimaryIndex::createInIterator(
    ResourceMonitor& monitor, transaction::Methods* trx,
    aql::AstNode const* attrNode, aql::AstNode const* valNode, bool ascending) {
  // _key or _id?
  bool const isId = (attrNode->stringEquals(StaticStrings::IdString));

  TRI_ASSERT(valNode->isArray());

  VPackBuilder keysBuilder;

  fillInLookupValues(trx, keysBuilder, valNode, ascending, isId);
  return std::make_unique<RocksDBPrimaryIndexInIterator>(
      monitor, &_collection, trx, this, std::move(keysBuilder), isId);
}

/// @brief create the iterator, for a single attribute, EQ operator
std::unique_ptr<IndexIterator> RocksDBPrimaryIndex::createEqIterator(
    ResourceMonitor& monitor, transaction::Methods* trx,
    aql::AstNode const* attrNode, aql::AstNode const* valNode,
    ReadOwnWrites readOwnWrites) {
  // _key or _id?
  bool const isId = (attrNode->stringEquals(StaticStrings::IdString));

  VPackBuilder keyBuilder;

  // handle the sole element
  handleValNode(trx, keyBuilder, valNode, isId);

  TRI_IF_FAILURE("PrimaryIndex::noIterator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (!keyBuilder.isEmpty()) {
    return std::make_unique<RocksDBPrimaryIndexEqIterator>(
        monitor, &_collection, trx, this, std::move(keyBuilder), isId,
        readOwnWrites);
  }

  return std::make_unique<EmptyIndexIterator>(&_collection, trx);
}

void RocksDBPrimaryIndex::fillInLookupValues(transaction::Methods* trx,
                                             VPackBuilder& keys,
                                             aql::AstNode const* values,
                                             bool ascending, bool isId) const {
  TRI_ASSERT(values != nullptr);
  TRI_ASSERT(values->type == aql::NODE_TYPE_ARRAY);

  keys.clear();
  keys.openArray();

  size_t const n = values->numMembers();

  // only leave the valid elements
  if (ascending) {
    for (size_t i = 0; i < n; ++i) {
      handleValNode(trx, keys, values->getMemberUnchecked(i), isId);
      TRI_IF_FAILURE("PrimaryIndex::iteratorValNodes") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    }
  } else {
    size_t i = n;
    while (i > 0) {
      --i;
      handleValNode(trx, keys, values->getMemberUnchecked(i), isId);
      TRI_IF_FAILURE("PrimaryIndex::iteratorValNodes") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }
    }
  }

  TRI_IF_FAILURE("PrimaryIndex::noIterator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  keys.close();
}

/// @brief add a single value node to the iterator's keys
void RocksDBPrimaryIndex::handleValNode(transaction::Methods* trx,
                                        VPackBuilder& keys,
                                        aql::AstNode const* valNode,
                                        bool isId) const {
  if (!valNode->isStringValue() || valNode->getStringLength() == 0) {
    return;
  }

  if (isId) {
    // lookup by _id. now validate if the lookup is performed for the
    // correct collection (i.e. _collection)
    char const* key = nullptr;
    size_t outLength = 0;
    std::shared_ptr<LogicalCollection> collection;
    Result res =
        trx->resolveId(valNode->getStringValue(), valNode->getStringLength(),
                       collection, key, outLength);

    if (!res.ok()) {
      return;
    }

    TRI_ASSERT(collection != nullptr);
    TRI_ASSERT(key != nullptr);

    bool isRunningInCluster = ServerState::instance()->isRunningInCluster();

    if (!isRunningInCluster && collection->id() != _collection.id()) {
      // only continue lookup if the id value is syntactically correct and
      // refers to "our" collection, using local collection id
      return;
    }

    if (isRunningInCluster) {
#ifdef USE_ENTERPRISE
      if (collection->isSmart() && collection->type() == TRI_COL_TYPE_EDGE) {
        auto c = dynamic_cast<VirtualClusterSmartEdgeCollection const*>(
            collection.get());
        if (c == nullptr) {
          THROW_ARANGO_EXCEPTION_MESSAGE(
              TRI_ERROR_INTERNAL, "unable to cast smart edge collection");
        }

        if (!c->isDisjoint() && (_collection.planId() != c->getLocalCid() &&
                                 _collection.planId() != c->getFromCid() &&
                                 _collection.planId() != c->getToCid())) {
          // invalid planId
          return;
        } else if (c->isDisjoint() &&
                   _collection.planId() != c->getLocalCid()) {
          // invalid planId
          return;
        }
      } else
#endif
          if (collection->planId() != _collection.planId()) {
        // only continue lookup if the id value is syntactically correct and
        // refers to "our" collection, using cluster collection id
        return;
      }
    }

    // use _key value from _id
    keys.add(VPackValuePair(key, outLength, VPackValueType::String));
  } else {
    keys.add(VPackValuePair(valNode->getStringValue(),
                            valNode->getStringLength(),
                            VPackValueType::String));
  }
}

namespace {

struct RocksDBPrimaryIndexStreamIterator final : AqlIndexStreamIterator {
  RocksDBPrimaryIndex const* _index;
  std::unique_ptr<rocksdb::Iterator> _iterator;

  VPackBuilder _builder;
  VPackString _cache;
  RocksDBKeyBounds _bounds;
  rocksdb::Slice _end;
  RocksDBKey _rocksdbKey;

  RocksDBPrimaryIndexStreamIterator(RocksDBPrimaryIndex const* index,
                                    transaction::Methods* trx)
      : _index(index),
        _bounds(RocksDBKeyBounds::PrimaryIndex(
            index->objectId(), KeyGeneratorHelper::lowestKey,
            KeyGeneratorHelper::highestKey)) {
    _end = _bounds.end();
    RocksDBTransactionMethods* mthds =
        RocksDBTransactionState::toMethods(trx, index->collection().id());
    _iterator = mthds->NewIterator(index->columnFamily(), [&](auto& opts) {
      TRI_ASSERT(opts.prefix_same_as_start);
      opts.iterate_upper_bound = &_end;
    });
    seekInternal({});
  }

  bool position(std::span<VPackSlice> span) const override {
    if (!_iterator->Valid()) {
      return false;
    }

    TRI_ASSERT(span.size() == 1);
    // store the actual key
    span[0] = _builder.slice();
    return true;
  }

  void seekInternal(std::string_view key) {
    if (key.empty()) {
      _iterator->Seek(_bounds.start());
    } else {
      _rocksdbKey.constructPrimaryIndexValue(_index->objectId(), key);
      _iterator->Seek(_rocksdbKey.string());
    }

    if (_iterator->Valid()) {
      auto keySlice = RocksDBKey::primaryKey(_iterator->key());
      _builder.clear();
      _builder.add(VPackValue(std::string{keySlice.begin(), keySlice.end()}));
    }
  }

  bool seek(std::span<VPackSlice> span) override {
    TRI_ASSERT(span.size() == 1 && span[0].isString());
    seekInternal(span[0].stringView());

    return position(span);
  }

  LocalDocumentId load(std::span<VPackSlice> projections) const override {
    TRI_ASSERT(_iterator->Valid());

    for (auto& slice : projections) {
      slice = _builder.slice();
    }

    return RocksDBValue::documentId(_iterator->value());
  }

  bool next(std::span<VPackSlice> key, LocalDocumentId& docId,
            std::span<VPackSlice> projections) override {
    _iterator->Next();

    if (!_iterator->Valid()) {
      return false;
    }
    auto keySlice = RocksDBKey::primaryKey(_iterator->key());
    _builder.clear();
    _builder.add(VPackValue(std::string{keySlice.begin(), keySlice.end()}));
    key[0] = _builder.slice();
    docId = load(projections);

    return true;
  }

  void cacheCurrentKey(std::span<VPackSlice> cache) override {
    _cache = VPackString{_builder.slice()};
    cache[0] = _cache;
  }

  bool reset(std::span<VPackSlice> span,
             std::span<VPackSlice> constants) override {
    TRI_ASSERT(constants.empty());
    seekInternal({});
    return position(span);
  }
};

}  // namespace

std::unique_ptr<AqlIndexStreamIterator> RocksDBPrimaryIndex::streamForCondition(
    transaction::Methods* trx, IndexStreamOptions const& opts) {
  if (!supportsStreamInterface(opts)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "RocksDBPrimaryIndex streamForCondition was "
                                   "called with unsupported options.");
  }

  auto stream = [&]() -> std::unique_ptr<AqlIndexStreamIterator> {
    TRI_ASSERT(isSorted());
    return std::make_unique<RocksDBPrimaryIndexStreamIterator>(this, trx);
  }();

  return stream;
}

bool RocksDBPrimaryIndex::checkSupportsStreamInterface(
    std::vector<std::vector<basics::AttributeName>> const& coveredFields,
    IndexStreamOptions const& streamOpts) noexcept {
  // we can only project values that are in range
  TRI_ASSERT(coveredFields.size() == 2);
  TRI_ASSERT(coveredFields[0][0].name == StaticStrings::KeyString &&
             coveredFields[1][0].name == StaticStrings::IdString);

  if (!streamOpts.constantFields.empty()) {
    return false;
  }

  for (auto idx : streamOpts.projectedFields) {
    if (idx != 0) {
      return false;
    }
  }

  // For the primary index, there is only one property set, which is "_key".
  if (streamOpts.usedKeyFields.size() != 1) {
    return false;
  }

  if (streamOpts.usedKeyFields[0] != 0) {
    return false;
  }

  return true;
}

bool RocksDBPrimaryIndex::supportsStreamInterface(
    IndexStreamOptions const& streamOpts) const noexcept {
  return checkSupportsStreamInterface(_coveredFields, streamOpts);
}
