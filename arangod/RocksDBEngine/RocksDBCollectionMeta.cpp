////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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

#include "RocksDBCollectionMeta.h"

#include "Basics/ReadLocker.h"
#include "Basics/WriteLocker.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "RocksDBEngine/RocksDBColumnFamily.h"
#include "RocksDBEngine/RocksDBCuckooIndexEstimator.h"
#include "RocksDBEngine/RocksDBIndex.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"

#include <rocksdb/db.h>
#include <rocksdb/utilities/transaction.h>

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

RocksDBCollectionMeta::DocCount::DocCount(VPackSlice const& slice)
    : _committedSeq(0), _added(0), _removed(0), _revisionId(0) {
  if (!slice.isArray()) {
    // got a somewhat invalid slice. probably old data from before the key
    // structure changes
    return;
  }

  VPackArrayIterator array(slice);
  if (array.valid()) {
    this->_committedSeq = (*array).getUInt();
    // versions pre 3.4 stored only a single "count" value
    // 3.4 and higher store "added" and "removed" seperately
    this->_added = (*(++array)).getUInt();
    if (array.size() > 3) {
      TRI_ASSERT(array.size() == 4);
      this->_removed = (*(++array)).getUInt();
    }
    this->_revisionId = (*(++array)).getUInt();
  }
}

void RocksDBCollectionMeta::DocCount::toVelocyPack(VPackBuilder& b) const {
  b.openArray();
  b.add(VPackValue(_committedSeq));
  b.add(VPackValue(_added));
  b.add(VPackValue(_removed));
  b.add(VPackValue(_revisionId));
  b.close();
}

RocksDBCollectionMeta::RocksDBCollectionMeta() : _count(0, 0, 0, 0) {}

/**
 * @brief Place a blocker to allow proper commit/serialize semantics
 *
 * Should be called immediately prior to internal RocksDB commit. If the
 * commit succeeds, any inserts/removals should be buffered, then the blocker
 * removed; otherwise simply remove the blocker.
 *
 * @param  trxId The identifier for the active transaction
 * @param  seq   The sequence number immediately prior to call
 * @return       May return error if we fail to allocate and place blocker
 */
Result RocksDBCollectionMeta::placeBlocker(TRI_voc_tid_t trxId, rocksdb::SequenceNumber seq) {
  return basics::catchToResult([&]() -> Result {
    Result res;
    WRITE_LOCKER(locker, _blockerLock);

    TRI_ASSERT(_blockers.end() == _blockers.find(trxId));
    TRI_ASSERT(_blockersBySeq.end() == _blockersBySeq.find(std::make_pair(seq, trxId)));

    auto insert = _blockers.emplace(trxId, seq);
    auto crosslist = _blockersBySeq.emplace(seq, trxId);
    if (!insert.second || !crosslist.second) {
      return res.reset(TRI_ERROR_INTERNAL);
    }
    LOG_TOPIC("1587a", TRACE, Logger::ENGINES)
        << "[" << this << "] placed blocker (" << trxId << ", " << seq << ")";
    return res;
  });
}

/**
 * @brief Removes an existing transaction blocker
 *
 * Should be called after transaction abort/rollback, or after buffering any
 * updates in case of successful commit. If no blocker exists with the
 * specified transaction identifier, then this will simply do nothing.
 *
 * @param trxId Identifier for active transaction (should match input to
 *              earlier `placeBlocker` call)
 */
void RocksDBCollectionMeta::removeBlocker(TRI_voc_tid_t trxId) {
  WRITE_LOCKER(locker, _blockerLock);
  auto it = _blockers.find(trxId);
  if (ADB_LIKELY(_blockers.end() != it)) {
    auto cross = _blockersBySeq.find(std::make_pair(it->second, it->first));
    TRI_ASSERT(_blockersBySeq.end() != cross);
    if (ADB_LIKELY(_blockersBySeq.end() != cross)) {
      _blockersBySeq.erase(cross);
    }
    _blockers.erase(it);
    LOG_TOPIC("1587b", TRACE, Logger::ENGINES)
        << "[" << this << "] removed blocker (" << trxId << ")";
  }
}

/// @brief returns the largest safe seq to squash updates against
rocksdb::SequenceNumber RocksDBCollectionMeta::committableSeq(rocksdb::SequenceNumber maxCommitSeq) const {
  READ_LOCKER(locker, _blockerLock);
  // if we have a blocker use the lowest counter
  rocksdb::SequenceNumber committable = maxCommitSeq;
  if (!_blockersBySeq.empty()) {
    auto it = _blockersBySeq.begin();
    committable = std::min(it->first, maxCommitSeq);
  }
  LOG_TOPIC("1587c", TRACE, Logger::ENGINES)
      << "[" << this << "] committableSeq determined to be " << committable;
  return committable;
}

bool RocksDBCollectionMeta::applyAdjustments(rocksdb::SequenceNumber commitSeq) {
  bool didWork = false;

  decltype(_bufferedAdjs) swapper;
  {
    std::lock_guard<std::mutex> guard(_bufferLock);
    if (_stagedAdjs.empty()) {
      _stagedAdjs.swap(_bufferedAdjs);
    } else {
      swapper.swap(_bufferedAdjs);
    }
  }
  if (!swapper.empty()) {
    _stagedAdjs.insert(swapper.begin(), swapper.end());
  }

  auto it = _stagedAdjs.begin();
  while (it != _stagedAdjs.end() && it->first <= commitSeq) {
    LOG_TOPIC("1487a", TRACE, Logger::ENGINES)
        << "[" << this << "] applying counter adjustment (" << it->first << ", "
        << it->second.adjustment << ", " << it->second.revisionId << ")";
    if (it->second.adjustment > 0) {
      _count._added += it->second.adjustment;
    } else if (it->second.adjustment < 0) {
      _count._removed += -(it->second.adjustment);
    }
    TRI_ASSERT(_count._added >= _count._removed);
    if (it->second.revisionId != 0) {
      _count._revisionId = it->second.revisionId;
    }
    it = _stagedAdjs.erase(it);
    didWork = true;
  }
  _count._committedSeq = commitSeq;
  return didWork;
}

/// @brief get the current count
RocksDBCollectionMeta::DocCount RocksDBCollectionMeta::loadCount() {
  return _count;
}

/// @brief buffer a counter adjustment
void RocksDBCollectionMeta::adjustNumberDocuments(rocksdb::SequenceNumber seq,
                                                  TRI_voc_rid_t revId, int64_t adj) {
  TRI_ASSERT(seq != 0 && (adj || revId));
  TRI_ASSERT(seq > _count._committedSeq);
  std::lock_guard<std::mutex> guard(_bufferLock);
  _bufferedAdjs.emplace(seq, Adjustment{revId, adj});
  LOG_TOPIC("1587d", TRACE, Logger::ENGINES)
      << "[" << this << "] buffered adjustment (" << seq << ", " << adj << ", "
      << revId << ")";
}

/// @brief buffer a counter adjustment ONLY in recovery, optimized to use less memory
bool RocksDBCollectionMeta::adjustNumberDocumentsInRecovery(rocksdb::SequenceNumber seq,
                                                            TRI_voc_rid_t revId, int64_t adj) {
  TRI_ASSERT(seq != 0 && (adj || revId));
  if (seq <= _count._committedSeq) {
    // already incorporated into counter
    return false;
  }
  TRI_ASSERT(seq > _count._committedSeq);
  if (_bufferedAdjs.empty()) {
    _bufferedAdjs.emplace(seq, Adjustment{revId, adj});
  } else {
    // in recovery we should only maintain one adjustment which combines
    TRI_ASSERT(_bufferedAdjs.size() == 1);
    auto old = _bufferedAdjs.begin();
    TRI_ASSERT(old != _bufferedAdjs.end());
    if (old->first <= seq) {
      old->second.adjustment += adj;
      // just adjust counter, not rev
    } else {
      _bufferedAdjs.emplace(seq, Adjustment{revId, adj + old->second.adjustment});
      _bufferedAdjs.erase(old);
    }
  }
  LOG_TOPIC("1587e", TRACE, Logger::ENGINES)
      << "[" << this << "] buffered adjustment (" << seq << ", " << adj << ", "
      << revId << ") in recovery";
  return true;
}


/// @brief serialize the collection metadata
Result RocksDBCollectionMeta::serializeMeta(rocksdb::WriteBatch& batch,
                                            LogicalCollection& coll, bool force,
                                            VPackBuilder& tmp,
                                            rocksdb::SequenceNumber& appliedSeq) {
  TRI_ASSERT(appliedSeq != UINT64_MAX);
  
  Result res;
  if (coll.deleted()) {
    return res;
  }

  const rocksdb::SequenceNumber maxCommitSeq = committableSeq(appliedSeq);
  bool didWork = applyAdjustments(maxCommitSeq);
  TRI_ASSERT(maxCommitSeq <= appliedSeq);
  TRI_ASSERT(maxCommitSeq != UINT64_MAX);
  appliedSeq = maxCommitSeq;

  RocksDBKey key;
  rocksdb::ColumnFamilyHandle* const cf = RocksDBColumnFamily::definitions();
  RocksDBCollection* const rcoll = static_cast<RocksDBCollection*>(coll.getPhysical());

  // Step 1. store the document count
  tmp.clear();
  if (didWork || force) {
    _count.toVelocyPack(tmp);
    key.constructCounterValue(rcoll->objectId());
    rocksdb::Slice value((char*)tmp.start(), tmp.size());
    rocksdb::Status s = batch.Put(cf, key.string(), value);
    if (!s.ok()) {
      LOG_TOPIC("1d7f3", WARN, Logger::ENGINES)
          << "[" << this << "] writing counter for collection with objectId '"
          << rcoll->objectId() << "' failed: " << s.ToString();
      return res.reset(rocksutils::convertStatus(s));
    } else {
      LOG_TOPIC("1387a", TRACE, Logger::ENGINES)
          << "[" << this << "] wrote counter '" << tmp.toJson()
          << "' for collection with objectId '" << rcoll->objectId() << "'";
     }
  } else {
    LOG_TOPIC("1e7f3", TRACE, Logger::ENGINES)
        << "[" << this << "] not writing counter for collection with "
        << "objectId '" << rcoll->objectId() << "', no updates applied";
   }

  // Step 2. store the key generator
  KeyGenerator* keyGen = coll.keyGenerator();
  if ((didWork || force) && keyGen->hasDynamicState()) {
    // only a key generator with dynamic data needs to be recovered
    key.constructKeyGeneratorValue(rcoll->objectId());

    tmp.clear();
    tmp.openObject();
    keyGen->toVelocyPack(tmp);
    tmp.close();

    RocksDBValue value = RocksDBValue::KeyGeneratorValue(tmp.slice());
    rocksdb::Status s = batch.Put(cf, key.string(), value.string());
    LOG_TOPIC("17610", TRACE, Logger::ENGINES)
        << "[" << this << "] writing key generator coll " << coll.name();

    if (!s.ok()) {
      LOG_TOPIC("333fe", WARN, Logger::ENGINES)
          << "[" << this << "] writing key generator data failed";
      return res.reset(rocksutils::convertStatus(s));
    }
  }

  // Step 3. store the index estimates
  std::string output;
  auto indexes = coll.getIndexes();
  for (std::shared_ptr<arangodb::Index>& index : indexes) {
    RocksDBIndex* idx = static_cast<RocksDBIndex*>(index.get());
    RocksDBCuckooIndexEstimator<uint64_t>* est = idx->estimator();
    if (est == nullptr) {  // does not have an estimator
      LOG_TOPIC("ab329", TRACE, Logger::ENGINES)
          << "[" << this << "] index '" << idx->objectId()
          << "' does not have an estimator";
      continue;
    }

    if (est->needToPersist() || force) {
      LOG_TOPIC("82a07", TRACE, Logger::ENGINES)
          << "[" << this << "] beginning estimate serialization for index '"
          << idx->objectId() << "'";
      output.clear();

      est->serialize(output, maxCommitSeq);
      TRI_ASSERT(output.size() > sizeof(uint64_t));

      LOG_TOPIC("6b761", TRACE, Logger::ENGINES)
          << "[" << this << "] serialized estimate for index '"
          << idx->objectId() << "' with estimate " << est->computeEstimate()
          << " valid through seq " << appliedSeq;

      key.constructIndexEstimateValue(idx->objectId());
      rocksdb::Slice value(output);
      rocksdb::Status s = batch.Put(cf, key.string(), value);
      if (!s.ok()) {
        LOG_TOPIC("ff233", WARN, Logger::ENGINES)
            << "[" << this << "] writing index estimates failed";
        return res.reset(rocksutils::convertStatus(s));
      }
    } else {
      LOG_TOPIC("ab328", TRACE, Logger::ENGINES)
          << "[" << this << "] index '" << idx->objectId()
          << "' estimator does not need to be persisted";
    }
  }

  return res;
}

/// @brief deserialize collection metadata, only called on startup
Result RocksDBCollectionMeta::deserializeMeta(rocksdb::DB* db, LogicalCollection& coll) {
  RocksDBCollection* rcoll = static_cast<RocksDBCollection*>(coll.getPhysical());

  // Step 1. load the counter
  auto cf = RocksDBColumnFamily::definitions();
  rocksdb::ReadOptions ro;
  ro.fill_cache = false;

  RocksDBKey key;
  key.constructCounterValue(rcoll->objectId());

  rocksdb::PinnableSlice value;
  rocksdb::Status s = db->Get(ro, cf, key.string(), &value);
  if (s.ok()) {
    VPackSlice countSlice = RocksDBValue::data(value);
    _count = RocksDBCollectionMeta::DocCount(countSlice);
    LOG_TOPIC("1387b", TRACE, Logger::ENGINES)
        << "[" << this << "] recovered counter '" << countSlice.toJson()
        << "' for collection with objectId '" << rcoll->objectId() << "'";
  } else if (!s.IsNotFound()) {
    LOG_TOPIC("1397c", TRACE, Logger::ENGINES)
        << "[" << this << "] error while recovering counter for collection with objectId '"
        << rcoll->objectId() << "': " << rocksutils::convertStatus(s).errorMessage();
    return rocksutils::convertStatus(s);
  } else {
    LOG_TOPIC("1387c", TRACE, Logger::ENGINES)
        << "[" << this << "] no counter found for collection with objectId '"
        << rcoll->objectId() << "'";
  }

  // Step 2. load the key generator
  KeyGenerator* keyGen = coll.keyGenerator();
  if (keyGen->hasDynamicState()) {
    // only a key generator with dynamic data needs to be recovered
    key.constructKeyGeneratorValue(rcoll->objectId());
    s = db->Get(ro, cf, key.string(), &value);
    if (s.ok()) {
      VPackSlice keyGenProps = RocksDBValue::data(value);
      TRI_ASSERT(keyGenProps.isObject());
      // simon: wtf who decided this is a good deserialization routine ?!
      VPackSlice val = keyGenProps.get(StaticStrings::LastValue);
      if (val.isString()) {
        VPackValueLength size;
        const char* data = val.getString(size);
        keyGen->track(data, size);
      } else if (val.isInteger()) {
        uint64_t lastValue = val.getUInt();
        std::string str = std::to_string(lastValue);
        keyGen->track(str.data(), str.size());
      }

    } else if (!s.IsNotFound()) {
      return rocksutils::convertStatus(s);
    }
  }

  // Step 3. load the index estimates
  auto indexes = coll.getIndexes();
  for (std::shared_ptr<arangodb::Index>& index : indexes) {
    RocksDBIndex* idx = static_cast<RocksDBIndex*>(index.get());
    if (idx->estimator() == nullptr) {
      continue;
    }

    key.constructIndexEstimateValue(idx->objectId());
    s = db->Get(ro, cf, key.string(), &value);
    if (!s.ok() && !s.IsNotFound()) {
      return rocksutils::convertStatus(s);
    } else if (s.IsNotFound()) {  // expected with nosync recovery tests
      LOG_TOPIC("ecdbb", WARN, Logger::ENGINES)
          << "[" << this << "] recalculating index estimate for index "
          << "type '" << idx->typeName() << "' with id '" << idx->id() << "'";
      idx->recalculateEstimates();
      continue;
    }

    arangodb::velocypack::StringRef estimateInput(value.data(), value.size());
    if (RocksDBCuckooIndexEstimator<uint64_t>::isFormatSupported(estimateInput)) {
      TRI_ASSERT(rocksutils::uint64FromPersistent(value.data()) <= db->GetLatestSequenceNumber());

      auto est = std::make_unique<RocksDBCuckooIndexEstimator<uint64_t>>(estimateInput);
      LOG_TOPIC("63f3b", DEBUG, Logger::ENGINES)
          << "[" << this << "] found index estimator for objectId '"
          << idx->objectId() << "' committed seqNr '" << est->appliedSeq()
          << "' with estimate " << est->computeEstimate();

      idx->setEstimator(std::move(est));
    } else {
      LOG_TOPIC("dcd98", ERR, Logger::ENGINES)
          << "[" << this << "] unsupported index estimator format in index "
          << "with objectId '" << idx->objectId() << "'";
    }
  }

  return Result();
}

// static helper methods to modify collection meta entries in rocksdb

/// @brief load collection
/*static*/ RocksDBCollectionMeta::DocCount RocksDBCollectionMeta::loadCollectionCount(
    rocksdb::DB* db, uint64_t objectId) {
  auto cf = RocksDBColumnFamily::definitions();
  rocksdb::ReadOptions ro;
  ro.fill_cache = false;

  RocksDBKey key;
  key.constructCounterValue(objectId);

  rocksdb::PinnableSlice value;
  rocksdb::Status s = db->Get(ro, cf, key.string(), &value);
  if (s.ok()) {
    VPackSlice countSlice = RocksDBValue::data(value);
    LOG_TOPIC("1387e", TRACE, Logger::ENGINES)
        << "loaded counter '" << countSlice.toJson()
        << "' for collection with objectId '" << objectId << "'";
    return RocksDBCollectionMeta::DocCount(countSlice);
  }
  LOG_TOPIC("1387f", TRACE, Logger::ENGINES)
      << "loaded default zero counter for collection with objectId '" << objectId << "'";
  return DocCount(0, 0, 0, 0);
}

/// @brief remove collection metadata
/*static*/ Result RocksDBCollectionMeta::deleteCollectionMeta(rocksdb::DB* db,
                                                              uint64_t objectId) {
  rocksdb::ColumnFamilyHandle* const cf = RocksDBColumnFamily::definitions();
  rocksdb::WriteOptions wo;

  // Step 1. delete the document count
  RocksDBKey key;
  key.constructCounterValue(objectId);
  rocksdb::Status s = db->Delete(wo, cf, key.string());
  if (!s.ok()) {
    LOG_TOPIC("93718", ERR, Logger::ENGINES)
        << "could not delete counter value for collection with objectId '"
        << objectId << "': " << s.ToString();
    // try to remove the key generator value regardless
  } else {
    LOG_TOPIC("93719", TRACE, Logger::ENGINES)
        << "deleted counter for collection with objectId '" << objectId << "'";
  }

  key.constructKeyGeneratorValue(objectId);
  s = db->Delete(wo, cf, key.string());
  if (!s.ok() && !s.IsNotFound()) {
    LOG_TOPIC("af3dc", ERR, Logger::ENGINES)
        << "could not delete key generator value: " << s.ToString();
    return rocksutils::convertStatus(s);
  }

  return Result();
}

/// @brief remove collection index estimate
/*static*/ Result RocksDBCollectionMeta::deleteIndexEstimate(rocksdb::DB* db, uint64_t objectId) {
  rocksdb::ColumnFamilyHandle* const cf = RocksDBColumnFamily::definitions();
  rocksdb::WriteOptions wo;

  RocksDBKey key;
  key.constructIndexEstimateValue(objectId);
  rocksdb::Status s = db->Delete(wo, cf, key.string());
  if (!s.ok() && !s.IsNotFound()) {
    return rocksutils::convertStatus(s);
  }
  return Result();
}
