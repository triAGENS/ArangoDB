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

#include "RocksDBIterators.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Random/RandomGenerator.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBMetaCollection.h"
#include "RocksDBEngine/RocksDBTransactionMethods.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

using namespace arangodb;

namespace {
constexpr bool AnyIteratorFillBlockCache = false;
}  // namespace

/// @brief iterator over all documents in the collection
/// basically sorted after LocalDocumentId
template<bool mustCheckBounds>
class RocksDBAllIndexIterator final : public IndexIterator {
 public:
  RocksDBAllIndexIterator(LogicalCollection* collection,
                          transaction::Methods* trx,
                          ReadOwnWrites readOwnWrites)
      : IndexIterator(collection, trx, readOwnWrites),
        _bounds(static_cast<RocksDBMetaCollection*>(collection->getPhysical())
                    ->bounds()),
        _upperBound(_bounds.end()),
        _cmp(_bounds.columnFamily()->GetComparator()),
        _mustSeek(true),
        _bytesRead(0) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    rocksdb::ColumnFamilyDescriptor desc;
    _bounds.columnFamily()->GetDescriptor(&desc);
    TRI_ASSERT(desc.options.prefix_extractor);
#endif
  }

  ~RocksDBAllIndexIterator() {
    _trx->state()->trackShardUsage(
        *_trx->resolver(), _collection->vocbase().name(), _collection->name(),
        _trx->username(), AccessMode::Type::READ, "collection scan",
        _bytesRead);
  }

  std::string_view typeName() const noexcept override {
    return "all-index-iterator";
  }

  /// @brief index does not support rearming
  bool canRearm() const override { return false; }

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
      TRI_ASSERT(_bounds.objectId() == RocksDBKey::objectId(_iterator->key()));

      // do not count number of bytes read here, as the cb will likely read
      // the document itself
      cb(RocksDBKey::documentId(_iterator->key()));
      _iterator->Next();

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

  bool nextDocumentImpl(DocumentCallback const& cb, uint64_t limit) override {
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
      // count number of bytes read here
      _bytesRead += _iterator->value().size();
      // TODO(MBkkt) optimize it, extract value from rocksdb
      cb(RocksDBKey::documentId(_iterator->key()), nullptr,
         VPackSlice(
             reinterpret_cast<uint8_t const*>(_iterator->value().data())));
      _iterator->Next();

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
        --count;
        ++skipped;
        _iterator->Next();

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

  void resetImpl() final {
    TRI_ASSERT(_trx->state()->isRunning());
    _mustSeek = true;
  }

 private:
  bool outOfRange() const {
    if constexpr (mustCheckBounds) {
      TRI_ASSERT(_trx->state()->isRunning());
      TRI_ASSERT(_iterator != nullptr);
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

      // note: this is always a forward iterator
      return _cmp->Compare(_iterator->key(), _upperBound) > 0;
    } else {
      return false;
    }
  }

  void ensureIterator() {
    if (_iterator == nullptr) {
      // acquire rocksdb transaction
      auto* mthds = RocksDBTransactionState::toMethods(_trx, _collection->id());

      _iterator =
          mthds->NewIterator(_bounds.columnFamily(), [&](ReadOptions& ro) {
            TRI_ASSERT(ro.snapshot != nullptr);
            TRI_ASSERT(ro.prefix_same_as_start);
            ro.verify_checksums = false;  // TODO evaluate
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
              ro.iterate_upper_bound = &_upperBound;
            }
            ro.readOwnWrites = static_cast<bool>(canReadOwnWrites());
          });
    }

    TRI_ASSERT(_iterator != nullptr);
    if (_mustSeek) {
      _iterator->Seek(_bounds.start());
      _mustSeek = false;
    }
    TRI_ASSERT(!_mustSeek);
  }

  RocksDBKeyBounds const _bounds;
  rocksdb::Slice const _upperBound;  // used for iterate_upper_bound
  std::unique_ptr<rocksdb::Iterator> _iterator;
  rocksdb::Comparator const* _cmp;
  // we use _mustSeek to save repeated seeks for the same start key
  bool _mustSeek;
  size_t _bytesRead;
};

template<bool forward>
class RocksDBAnyIndexIterator final : public IndexIterator {
 public:
  RocksDBAnyIndexIterator(LogicalCollection* collection,
                          transaction::Methods* trx)
      : IndexIterator(collection, trx,
                      ReadOwnWrites::no),  // any-iterator never needs
                                           // to observe own writes.
        _cmp(RocksDBColumnFamilyManager::get(
                 RocksDBColumnFamilyManager::Family::Documents)
                 ->GetComparator()),
        _objectId(static_cast<RocksDBMetaCollection*>(collection->getPhysical())
                      ->objectId()),
        _bounds(static_cast<RocksDBMetaCollection*>(collection->getPhysical())
                    ->bounds()),
        _total(0),
        _returned(0),
        _bytesRead(0) {
    auto* mthds = RocksDBTransactionState::toMethods(trx, collection->id());
    _iterator = mthds->NewIterator(
        _bounds.columnFamily(), [](rocksdb::ReadOptions& options) {
          TRI_ASSERT(options.snapshot != nullptr);
          TRI_ASSERT(options.prefix_same_as_start);
          options.fill_cache = AnyIteratorFillBlockCache;
          options.verify_checksums = false;  // TODO evaluate
        });
    TRI_ASSERT(_iterator != nullptr);
    if (_iterator == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "invalid iterator in RocksDBAnyIndexIterator");
    }

    _total = collection->getPhysical()->numberDocuments(trx);
    reset();  // initial seek
  }

  ~RocksDBAnyIndexIterator() {
    _trx->state()->trackShardUsage(
        *_trx->resolver(), _collection->vocbase().name(), _collection->name(),
        _trx->username(), AccessMode::Type::READ, "collection any lookup",
        _bytesRead);
  }

  std::string_view typeName() const noexcept override {
    return "any-index-iterator";
  }

  bool nextImpl(LocalDocumentIdCallback const& cb, uint64_t limit) override {
    return doNext(limit,
                  [&]() { cb(RocksDBKey::documentId(_iterator->key())); });
  }

  bool nextDocumentImpl(DocumentCallback const& cb, uint64_t limit) override {
    return doNext(limit, [&]() {
      _bytesRead += _iterator->value().size();
      cb(RocksDBKey::documentId(_iterator->key()), nullptr,
         VPackSlice(
             reinterpret_cast<uint8_t const*>(_iterator->value().data())));
    });
  }

  // cppcheck-suppress virtualCallInConstructor ; desired impl
  void resetImpl() final {
    // the assumption is that we don't reset this iterator unless
    // it is out of range or invalid
    if (_total == 0 || (_iterator->Valid() && !outOfRange())) {
      return;
    }
    // scanning forward or backward with RocksDB is expensive. we
    // definitely don't want to scan a million keys here, so limit
    // the number of scan steps to some reasonable amount.
    uint64_t steps = RandomGenerator::interval(_total - 1) % 500;

    RocksDBKeyLeaser key(_trx);
    key->constructDocument(
        _objectId, LocalDocumentId(RandomGenerator::interval(UINT64_MAX)));
    _iterator->Seek(key->string());

    if (checkIter()) {
      while (steps-- > 0) {
        if constexpr (forward) {
          _iterator->Next();
        } else {
          _iterator->Prev();
        }
        if (!checkIter()) {
          break;
        }
      }
    }

    // validate that Iterator is in a good shape and hasn't failed
    rocksutils::checkIteratorStatus(*_iterator);
  }

 private:
  template<typename Func>
  bool doNext(uint64_t limit, Func const& func) {
    TRI_ASSERT(_trx->state()->isRunning());

    if (limit == 0 || !_iterator->Valid() || outOfRange()) {
      // No limit no data, or we are actually done. The last call should have
      // returned false
      TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
      // validate that Iterator is in a good shape and hasn't failed
      rocksutils::checkIteratorStatus(*_iterator);
      return false;
    }

    while (limit > 0) {
      func();
      --limit;
      _returned++;
      _iterator->Next();
      if (!_iterator->Valid() || outOfRange()) {
        // validate that Iterator is in a good shape and hasn't failed
        rocksutils::checkIteratorStatus(*_iterator);
        if (_returned < _total) {
          _iterator->Seek(_bounds.start());
          continue;
        }
        return false;
      }
    }
    return true;
  }

  bool outOfRange() const {
    return _cmp->Compare(_iterator->key(), _bounds.end()) > 0;
  }

  bool checkIter() {
    bool valid = _iterator->Valid();
    if (valid) {
      if constexpr (forward) {
        valid = _cmp->Compare(_iterator->key(), _bounds.end()) <= 0;
      } else {
        valid = _cmp->Compare(_iterator->key(), _bounds.start()) >= 0;
      }
    }
    if (!valid) {
      if constexpr (forward) {
        _iterator->Seek(_bounds.start());
      } else {
        _iterator->SeekForPrev(_bounds.end());
      }

      if (!_iterator->Valid()) {
        return false;
      }
    }
    return true;
  }

  rocksdb::Comparator const* _cmp;
  std::unique_ptr<rocksdb::Iterator> _iterator;
  uint64_t const _objectId;
  RocksDBKeyBounds const _bounds;

  uint64_t _total;
  uint64_t _returned;
  size_t _bytesRead;
};

RocksDBGenericIterator::RocksDBGenericIterator(rocksdb::TransactionDB* db,
                                               rocksdb::ReadOptions& options,
                                               RocksDBKeyBounds const& bounds)
    : _bounds(bounds),
      _options(options),
      _iterator(db->NewIterator(_options, _bounds.columnFamily())),
      _cmp(_bounds.columnFamily()->GetComparator()) {
  seek(_bounds.start());
}

bool RocksDBGenericIterator::hasMore() const {
  return _iterator->Valid() && !outOfRange();
}

bool RocksDBGenericIterator::outOfRange() const {
  return _cmp->Compare(_iterator->key(), _bounds.end()) > 0;
}

bool RocksDBGenericIterator::seek(rocksdb::Slice const& key) {
  _iterator->Seek(key);
  return hasMore();
}

bool RocksDBGenericIterator::next(GenericCallback const& cb, size_t limit) {
  // @params
  // limit - maximum number of documents

  TRI_ASSERT(limit > 0);  // Someone called with limit == 0. Api broken
  if (limit == 0) {
    // No limit no data, or we are actually done. The last call should have
    // returned false
    // validate that Iterator is in a good shape and hasn't failed
    rocksutils::checkIteratorStatus(*_iterator);
    return false;
  }

  while (limit > 0 && hasMore()) {
    TRI_ASSERT(_bounds.objectId() == RocksDBKey::objectId(_iterator->key()));

    if (!cb(_iterator->key(), _iterator->value())) {
      // stop iteration
      return false;
    }
    --limit;
    _iterator->Next();

    // validate that Iterator is in a good shape and hasn't failed
    rocksutils::checkIteratorStatus(*_iterator);
  }

  return hasMore();
}

RocksDBGenericIterator arangodb::createPrimaryIndexIterator(
    transaction::Methods* trx, LogicalCollection* col) {
  TRI_ASSERT(col != nullptr);
  TRI_ASSERT(trx != nullptr);

  auto* mthds = RocksDBTransactionState::toMethods(trx, col->id());

  rocksdb::ReadOptions options = mthds->iteratorReadOptions();
  TRI_ASSERT(options.snapshot != nullptr);  // trx must contain a valid snapshot
  TRI_ASSERT(options.prefix_same_as_start);
  options.fill_cache = false;
  options.verify_checksums = false;

  auto index = col->lookupIndex(
      IndexId::primary());  // RocksDBCollection->primaryIndex() is private
  TRI_ASSERT(index->type() == Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  auto primaryIndex = static_cast<RocksDBPrimaryIndex*>(index.get());

  auto bounds(RocksDBKeyBounds::PrimaryIndex(primaryIndex->objectId()));
  auto& engine = col->vocbase().engine<RocksDBEngine>();
  auto iterator = RocksDBGenericIterator(engine.db(), options, bounds);

  TRI_ASSERT(iterator.bounds().objectId() == primaryIndex->objectId());
  TRI_ASSERT(iterator.bounds().columnFamily() ==
             RocksDBColumnFamilyManager::get(
                 RocksDBColumnFamilyManager::Family::PrimaryIndex));
  return iterator;
}

namespace arangodb::rocksdb_iterators {
std::unique_ptr<IndexIterator> createAllIterator(LogicalCollection* collection,
                                                 transaction::Methods* trx,
                                                 ReadOwnWrites readOwnWrites) {
  bool mustCheckBounds =
      RocksDBTransactionState::toState(trx)->iteratorMustCheckBounds(
          collection->id(), readOwnWrites);
  if (mustCheckBounds) {
    return std::make_unique<RocksDBAllIndexIterator<true>>(collection, trx,
                                                           readOwnWrites);
  }
  return std::make_unique<RocksDBAllIndexIterator<false>>(collection, trx,
                                                          readOwnWrites);
}

std::unique_ptr<IndexIterator> createAnyIterator(LogicalCollection* collection,
                                                 transaction::Methods* trx) {
  bool forward = RandomGenerator::interval(uint16_t(1)) ? true : false;
  if (forward) {
    return std::make_unique<RocksDBAnyIndexIterator<true>>(collection, trx);
  }
  return std::make_unique<RocksDBAnyIndexIterator<false>>(collection, trx);
}

}  // namespace arangodb::rocksdb_iterators
