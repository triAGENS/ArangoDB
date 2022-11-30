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
/// @author Daniel H. Larkin
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBReplicationTailing.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Replication/common-defines.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Utils/CollectionGuard.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>
#include <rocksdb/write_batch.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rocksutils;

/// an incomplete convert function, basically only use for DDL ops
TRI_replication_operation_e rocksutils::convertLogType(RocksDBLogType t) {
  switch (t) {
    case RocksDBLogType::DatabaseCreate:
      return REPLICATION_DATABASE_CREATE;
    case RocksDBLogType::DatabaseDrop:
      return REPLICATION_DATABASE_DROP;
    case RocksDBLogType::CollectionCreate:
      return REPLICATION_COLLECTION_CREATE;
    case RocksDBLogType::CollectionDrop:
      return REPLICATION_COLLECTION_DROP;
    case RocksDBLogType::CollectionRename:
      return REPLICATION_COLLECTION_RENAME;
    case RocksDBLogType::CollectionChange:
      return REPLICATION_COLLECTION_CHANGE;
    case RocksDBLogType::CollectionTruncate:
      return REPLICATION_COLLECTION_TRUNCATE;
    case RocksDBLogType::IndexCreate:
      return REPLICATION_INDEX_CREATE;
    case RocksDBLogType::IndexChange:
      return REPLICATION_INDEX_CHANGE;
    case RocksDBLogType::IndexDrop:
      return REPLICATION_INDEX_DROP;
    case RocksDBLogType::ViewCreate:
      return REPLICATION_VIEW_CREATE;
    case RocksDBLogType::ViewDrop:
      return REPLICATION_VIEW_DROP;
    case RocksDBLogType::ViewChange:
      return REPLICATION_VIEW_CHANGE;
    case RocksDBLogType::BeginTransaction:
      return REPLICATION_TRANSACTION_START;
    case RocksDBLogType::CommitTransaction:
      return REPLICATION_TRANSACTION_COMMIT;

    default:
      TRI_ASSERT(false);
      return REPLICATION_INVALID;
  }
}

/// WAL parser
class WALParser final : public rocksdb::WriteBatch::Handler {
  // internal WAL parser states
  enum State : char {
    INVALID = 0,
    COLLECTION_CREATE,
    COLLECTION_DROP,
    COLLECTION_RENAME,
    COLLECTION_CHANGE,
    INDEX_CREATE,
    INDEX_DROP,
    VIEW_CREATE,
    VIEW_DROP,
    VIEW_CHANGE,
    VIEW_RENAME,
    TRANSACTION,
    SINGLE_PUT,
    SINGLE_REMOVE
  };

 public:
  WALParser(TRI_vocbase_t* vocbase, bool includeSystem,
            DataSourceId collectionId, VPackBuilder& builder)
      : _definitionsCF(RocksDBColumnFamilyManager::get(
                           RocksDBColumnFamilyManager::Family::Definitions)
                           ->GetID()),
        _documentsCF(RocksDBColumnFamilyManager::get(
                         RocksDBColumnFamilyManager::Family::Documents)
                         ->GetID()),
        _primaryCF(RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::PrimaryIndex)
                       ->GetID()),

        _vocbase(vocbase),
        _includeSystem(includeSystem),
        _onlyDataSourceId(collectionId),
        _builder(builder),
        _startSequence(0),
        _currentSequence(0),
        _lastEmittedTick(0) {}

  void LogData(rocksdb::Slice const& blob) override {
    RocksDBLogType type = RocksDBLogValue::type(blob);

    LOG_TOPIC("5a95b", TRACE, Logger::REPLICATION)
        << "[LOG] " << rocksDBLogTypeName(type);
    switch (type) {
      case RocksDBLogType::DatabaseCreate:  // not handled here
      case RocksDBLogType::DatabaseDrop: {
        resetTransientState();  // finish ongoing trx
        break;
      }
      case RocksDBLogType::CollectionCreate:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleCollection(RocksDBLogValue::databaseId(blob),
                                   RocksDBLogValue::collectionId(blob))) {
          _state = COLLECTION_CREATE;
        }
        break;
      case RocksDBLogType::CollectionRename:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleCollection(RocksDBLogValue::databaseId(blob),
                                   RocksDBLogValue::collectionId(blob))) {
          _state = COLLECTION_RENAME;
          _oldCollectionName =
              std::string(RocksDBLogValue::oldCollectionName(blob));
        }
        break;
      case RocksDBLogType::CollectionChange:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleCollection(RocksDBLogValue::databaseId(blob),
                                   RocksDBLogValue::collectionId(blob))) {
          _state = COLLECTION_CHANGE;
        }
        break;
      case RocksDBLogType::CollectionDrop: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        // always print drop collection marker, shouldHandleCollection will
        // always return false for dropped collections
        if (shouldHandleDB(dbid)) {
          {  // tick number
            std::string_view uuid = RocksDBLogValue::collectionUUID(blob);
            TRI_ASSERT(!uuid.empty());
            uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);
            VPackObjectBuilder marker(&_builder, true);
            marker->add("tick", VPackValue(std::to_string(tick)));
            marker->add("type", VPackValue(REPLICATION_COLLECTION_DROP));
            marker->add("database", VPackValue(std::to_string(dbid)));
            if (!uuid.empty()) {
              marker->add("cuid", VPackValuePair(uuid.data(), uuid.size(),
                                                 VPackValueType::String));
            }
            marker->add("cid", VPackValue(std::to_string(cid.id())));
            VPackObjectBuilder data(&_builder, "data", true);
            data->add("id", VPackValue(std::to_string(cid.id())));
            data->add("name", VPackValue(""));  // not used at all
          }
          updateLastEmittedTick(_currentSequence);
        }
        break;
      }
      case RocksDBLogType::CollectionTruncate: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        if (shouldHandleCollection(dbid, cid)) {
          TRI_ASSERT(_vocbase->id() == dbid);
          LogicalCollection* coll = loadCollection(cid);
          TRI_ASSERT(coll != nullptr);
          {
            uint64_t tick = _currentSequence;
            VPackObjectBuilder marker(&_builder, true);
            marker->add("tick", VPackValue(std::to_string(tick)));
            marker->add("type", VPackValue(REPLICATION_COLLECTION_TRUNCATE));
            marker->add("database", VPackValue(std::to_string(dbid)));
            marker->add("cuid", VPackValue(coll->guid()));
            marker->add("cid", VPackValue(std::to_string(cid.id())));
          }
          updateLastEmittedTick(_currentSequence);
        }
        break;
      }
      case RocksDBLogType::IndexChange: {
        resetTransientState();  // finish ongoing trx
        break;
      }
      case RocksDBLogType::IndexCreate: {
        resetTransientState();  // finish ongoing trx

        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);

        if (shouldHandleCollection(dbid, cid)) {
          TRI_ASSERT(_vocbase->id() == dbid);
          LogicalCollection* coll = loadCollection(cid);
          TRI_ASSERT(coll != nullptr);
          VPackSlice indexDef = RocksDBLogValue::indexSlice(blob);
          auto stripped = rocksutils::stripObjectIds(indexDef);
          uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);

          _builder.openObject();
          _builder.add("tick", VPackValue(std::to_string(tick)));
          _builder.add("type", VPackValue(REPLICATION_INDEX_CREATE));
          _builder.add("database", VPackValue(std::to_string(dbid)));
          _builder.add("cid", VPackValue(std::to_string(cid.id())));
          _builder.add("cuid", VPackValue(coll->guid()));
          _builder.add("cname", VPackValue(coll->name()));
          _builder.add("data", stripped.first);
          _builder.close();
          updateLastEmittedTick(tick);
        }

        break;
      }
      case RocksDBLogType::IndexDrop: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        IndexId iid = RocksDBLogValue::indexId(blob);
        // only print markers from this collection if it is set
        if (shouldHandleCollection(dbid, cid)) {
          TRI_ASSERT(_vocbase->id() == dbid);
          LogicalCollection* coll = loadCollection(cid);
          TRI_ASSERT(coll != nullptr);
          uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);
          _builder.openObject();
          _builder.add("tick", VPackValue(std::to_string(tick)));
          _builder.add("type", VPackValue(REPLICATION_INDEX_DROP));
          _builder.add("database", VPackValue(std::to_string(dbid)));
          _builder.add("cid", VPackValue(std::to_string(cid.id())));
          _builder.add("cname", VPackValue(coll->name()));
          _builder.add("data", VPackValue(VPackValueType::Object));
          _builder.add("id", VPackValue(std::to_string(iid.id())));
          _builder.close();
          _builder.close();
          updateLastEmittedTick(tick);
        }
        break;
      }
      case RocksDBLogType::ViewCreate:
      case RocksDBLogType::ViewDrop:
      case RocksDBLogType::ViewChange: {
        resetTransientState();  // finish ongoing trx
        break;
      }
      case RocksDBLogType::BeginTransaction: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        TransactionId tid = RocksDBLogValue::transactionId(blob);
        if (shouldHandleDB(dbid)) {
          _state = TRANSACTION;
          _currentTrxId = tid;
          _builder.openObject();
          _builder.add("tick", VPackValue(std::to_string(_currentSequence)));
          _builder.add("type", VPackValue(convertLogType(type)));
          _builder.add("database", VPackValue(std::to_string(dbid)));
          _builder.add("tid", VPackValue(std::to_string(tid.id())));
          _builder.close();
          updateLastEmittedTick(_currentSequence);
        }
        break;
      }
      case RocksDBLogType::CommitTransaction: {  // ideally optional
        if (_state == TRANSACTION) {
          TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
          TransactionId tid = RocksDBLogValue::transactionId(blob);
          TRI_ASSERT(_currentTrxId == tid && _vocbase->id() == dbid);
          if (shouldHandleDB(dbid) && _currentTrxId == tid) {
            writeCommitMarker();
          }
        }
        resetTransientState();
        break;
      }
      case RocksDBLogType::SinglePut: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        if (shouldHandleCollection(dbid, cid)) {
          _state = SINGLE_PUT;
        }
        break;
      }
      case RocksDBLogType::SingleRemove: {  // deprecated
        resetTransientState();              // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        if (shouldHandleCollection(dbid, cid)) {
          _state = SINGLE_REMOVE;  // revisionId is unknown
        }
        break;
      }
      case RocksDBLogType::DocumentRemoveV2: {  // remove within a trx
        if (_state == TRANSACTION) {
          TRI_ASSERT(_removedDocRid.empty());
          _removedDocRid = RocksDBLogValue::revisionId(blob);
        } else {
          resetTransientState();
        }
        break;
      }
      case RocksDBLogType::SingleRemoveV2: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        if (shouldHandleCollection(dbid, cid)) {
          _state = SINGLE_REMOVE;
          _removedDocRid = RocksDBLogValue::revisionId(blob);
        }
        break;
      }

      case RocksDBLogType::DocumentOperationsPrologue:
      case RocksDBLogType::DocumentRemove:
      case RocksDBLogType::DocumentRemoveAsPartOfUpdate:
      case RocksDBLogType::TrackedDocumentInsert:
      case RocksDBLogType::TrackedDocumentRemove:
      case RocksDBLogType::FlushSync:
        break;  // ignore deprecated && unused markers

      default:
        LOG_TOPIC("844da", WARN, Logger::REPLICATION)
            << "Unhandled wal log entry " << rocksDBLogTypeName(type);
        break;
    }
  }

  rocksdb::Status PutCF(uint32_t column_family_id, rocksdb::Slice const& key,
                        rocksdb::Slice const& value) override {
    tick();
    LOG_TOPIC("daa55", TRACE, Logger::REPLICATION)
        << "PUT: key:" << key.ToString() << "  value: " << value.ToString();

    if (column_family_id == _definitionsCF) {
      if (RocksDBKey::type(key) == RocksDBEntryType::Collection) {
        TRI_voc_tick_t dbid = RocksDBKey::databaseId(key);
        DataSourceId cid = RocksDBKey::collectionId(key);
        if (shouldHandleCollection(dbid, cid) &&
            (_state == COLLECTION_CREATE || _state == COLLECTION_RENAME ||
             _state == COLLECTION_CHANGE)) {
          TRI_ASSERT(_vocbase->id() == dbid);
          LogicalCollection* coll = loadCollection(cid);
          TRI_ASSERT(coll != nullptr);

          VPackSlice collectionDef = RocksDBValue::data(value);
          VPackObjectBuilder marker(&_builder, true);
          marker->add("tick", VPackValue(std::to_string(_currentSequence)));
          marker->add("database", VPackValue(std::to_string(dbid)));
          marker->add("cid", VPackValue(std::to_string(cid.id())));
          marker->add("cname", VPackValue(coll->name()));
          if (_state == COLLECTION_CREATE) {
            auto stripped = rocksutils::stripObjectIds(collectionDef);
            marker->add("type", VPackValue(REPLICATION_COLLECTION_CREATE));
            marker->add("data", stripped.first);
          } else if (_state == COLLECTION_RENAME) {
            marker->add("type", VPackValue(REPLICATION_COLLECTION_RENAME));
            VPackObjectBuilder data(&_builder, "data", true);
            data->add("name", VPackValue(coll->name()));
            data->add("oldName", VPackValue(_oldCollectionName));
            data->add("id", VPackValue(std::to_string(cid.id())));
          } else if (_state == COLLECTION_CHANGE) {
            auto stripped = rocksutils::stripObjectIds(collectionDef);
            marker->add("type", VPackValue(REPLICATION_COLLECTION_CHANGE));
            marker->add("data", stripped.first);
          }
          updateLastEmittedTick(_currentSequence);
        }
      }  // if (RocksDBKey::type(key) == RocksDBEntryType::Collection)

      // reset everything immediately after DDL operations
      resetTransientState();

    } else if (column_family_id == _documentsCF) {
      if (_state != TRANSACTION && _state != SINGLE_PUT) {
        resetTransientState();
        return rocksdb::Status();
      }
      TRI_ASSERT(_state != SINGLE_PUT || _currentTrxId.empty());
      TRI_ASSERT(_removedDocRid.empty());
      _removedDocRid = RevisionId::none();

      uint64_t objectId = RocksDBKey::objectId(key);
      auto dbCollPair = _vocbase->server()
                            .getFeature<EngineSelectorFeature>()
                            .engine<RocksDBEngine>()
                            .mapObjectToCollection(objectId);
      TRI_voc_tick_t const dbid = dbCollPair.first;
      DataSourceId const cid = dbCollPair.second;
      if (!shouldHandleCollection(dbid, cid)) {
        return rocksdb::Status();  // no reset here
      }
      TRI_ASSERT(_vocbase->id() == dbid);

      LogicalCollection* col = loadCollection(cid);
      TRI_ASSERT(col != nullptr);
      {
        VPackObjectBuilder marker(&_builder, true);
        marker->add("tick", VPackValue(std::to_string(_currentSequence)));
        marker->add("type", VPackValue(REPLICATION_MARKER_DOCUMENT));
        marker->add("database", VPackValue(std::to_string(dbid)));
        marker->add("tid", VPackValue(std::to_string(_currentTrxId.id())));
        marker->add("cid", VPackValue(std::to_string(cid.id())));
        marker->add("cname", VPackValue(col->name()));
        marker->add("data", RocksDBValue::data(value));
      }
      updateLastEmittedTick(_currentSequence);

      if (_state == SINGLE_PUT) {
        resetTransientState();  // always reset after single op
      }
    }

    return rocksdb::Status();
  }

  // for Delete / SingleDelete
  void handleDeleteCF(uint32_t cfId, rocksdb::Slice const& key) {
    tick();

    if (cfId != _primaryCF) {
      return;  // ignore all document operations
    }

    if (_state != TRANSACTION && _state != SINGLE_REMOVE) {
      resetTransientState();
      return;
    }
    TRI_ASSERT(_state != SINGLE_REMOVE || _currentTrxId.empty());

    uint64_t objectId = RocksDBKey::objectId(key);
    auto triple = _vocbase->server()
                      .getFeature<EngineSelectorFeature>()
                      .engine<RocksDBEngine>()
                      .mapObjectToIndex(objectId);
    TRI_voc_tick_t const dbid = std::get<0>(triple);
    DataSourceId const cid = std::get<1>(triple);
    if (!shouldHandleCollection(dbid, cid)) {
      _removedDocRid = RevisionId::none();  // ignore rid too
      return;                               // no reset here
    }
    TRI_ASSERT(_vocbase->id() == dbid);

    std::string_view docKey = RocksDBKey::primaryKey(key);
    LogicalCollection* coll = loadCollection(cid);
    TRI_ASSERT(coll != nullptr);
    {
      VPackObjectBuilder marker(&_builder, true);
      marker->add("tick", VPackValue(std::to_string(_currentSequence)));
      marker->add("type", VPackValue(REPLICATION_MARKER_REMOVE));
      marker->add("database", VPackValue(std::to_string(dbid)));
      marker->add("cid", VPackValue(std::to_string(cid.id())));
      marker->add("cname", VPackValue(coll->name()));
      marker->add("tid", VPackValue(std::to_string(_currentTrxId.id())));
      VPackObjectBuilder data(&_builder, "data", true);
      data->add(
          StaticStrings::KeyString,
          VPackValuePair(docKey.data(), docKey.size(), VPackValueType::String));
      data->add(StaticStrings::RevString,
                VPackValue(_removedDocRid.toString()));
    }
    updateLastEmittedTick(_currentSequence);
    _removedDocRid = RevisionId::none();  // always reset
    if (_state == SINGLE_REMOVE) {
      resetTransientState();
    }
  }

  rocksdb::Status DeleteCF(uint32_t column_family_id,
                           rocksdb::Slice const& key) override {
    handleDeleteCF(column_family_id, key);
    return rocksdb::Status();
  }

  rocksdb::Status SingleDeleteCF(uint32_t column_family_id,
                                 rocksdb::Slice const& key) override {
    handleDeleteCF(column_family_id, key);
    return rocksdb::Status();
  }

  rocksdb::Status DeleteRangeCF(uint32_t /*column_family_id*/,
                                const rocksdb::Slice& /*begin_key*/,
                                const rocksdb::Slice& /*end_key*/) override {
    // nothing special to-do here. collection dropping and
    // truncation is already handled elsewhere
    return rocksdb::Status();
  }

  rocksdb::Status MarkBeginPrepare(bool = false) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkBeginPrepare() handler not defined.");
  }

  rocksdb::Status MarkEndPrepare(rocksdb::Slice const& /*xid*/) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkEndPrepare() handler not defined.");
  }

  rocksdb::Status MarkNoop(bool empty_batch) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status MarkRollback(rocksdb::Slice const& /*xid*/) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkRollbackPrepare() handler not defined.");
  }

  rocksdb::Status MarkCommit(rocksdb::Slice const& /*xid*/) override {
    TRI_ASSERT(false);
    return rocksdb::Status::InvalidArgument(
        "MarkCommit() handler not defined.");
  }

  void startNewBatch(rocksdb::SequenceNumber startSequence) {
    // starting new write batch
    _startSequence = startSequence;
    _currentSequence = startSequence;
    _startOfBatch = true;
    // reset all states
    _state = INVALID;
    _currentTrxId = TransactionId::none();
    _removedDocRid = RevisionId::none();
    _oldCollectionName.clear();
  }

  void writeCommitMarker() {
    TRI_ASSERT(_state == TRANSACTION);
    LOG_TOPIC("e09eb", TRACE, Logger::REPLICATION)
        << "tick: " << _currentSequence << " commit transaction";

    _builder.openObject();
    _builder.add("tick", VPackValue(std::to_string(_currentSequence)));
    _builder.add(
        "type",
        VPackValue(static_cast<uint64_t>(REPLICATION_TRANSACTION_COMMIT)));
    _builder.add("database", VPackValue(std::to_string(_vocbase->id())));
    _builder.add("tid", VPackValue(std::to_string(_currentTrxId.id())));
    _builder.close();
    updateLastEmittedTick(_currentSequence);
    _state = INVALID;  // for safety
  }

  // should reset state flags which are only valid between
  // observing a specific log entry and a sequence of immediately
  // following PUT / DELETE / Log entries
  void resetTransientState() {
    // reset all states
    _state = INVALID;
    _currentTrxId = TransactionId::none();
    _removedDocRid = RevisionId::none();
    _oldCollectionName.clear();
  }

  uint64_t endBatch() {
    TRI_ASSERT(_removedDocRid.empty());
    TRI_ASSERT(_oldCollectionName.empty());
    resetTransientState();
    return _currentSequence;
  }

  rocksdb::SequenceNumber lastEmittedTick() const { return _lastEmittedTick; }

 private:
  void updateLastEmittedTick(rocksdb::SequenceNumber value) {
    // the tick values emitted should be always increasing
    // in the case of transaction we may see the same tick value as before, but
    // tick values must never decrease
    TRI_ASSERT(value >= _lastEmittedTick);
    _lastEmittedTick = value;
  }

  // tick function that is called before each new WAL entry
  void tick() {
    if (_startOfBatch) {
      // we are at the start of a batch. do NOT increase sequence number
      _startOfBatch = false;
    } else {
      // we are inside a batch already. now increase sequence number
      ++_currentSequence;
    }
  }

  bool shouldHandleDB(TRI_voc_tick_t dbid) const {
    return _vocbase->id() == dbid;
  }

  /// @brief Check if collection is in filter, will load collection
  bool shouldHandleCollection(TRI_voc_tick_t dbid, DataSourceId cid) {
    if (dbid == 0 || cid.empty() || !shouldHandleDB(dbid)) {
      return false;
    }
    if (_onlyDataSourceId.empty() || _onlyDataSourceId == cid) {
      LogicalCollection* collection = loadCollection(cid);
      if (collection == nullptr) {
        return false;
      }
      return !TRI_ExcludeCollectionReplication(collection->name(),
                                               _includeSystem,
                                               /*includeFoxxQueues*/ false);
    }
    return false;
  }

  LogicalCollection* loadCollection(DataSourceId cid) {
    TRI_ASSERT(cid.isSet());
    if (_vocbase != nullptr) {
      auto const& it = _collectionCache.find(cid);
      if (it != _collectionCache.end()) {
        return it->second.collection();
      }

      try {
        auto it = _collectionCache.try_emplace(cid, _vocbase, cid).first;
        return (*it).second.collection();
      } catch (...) {
        // collection not found
      }
    }

    return nullptr;
  }

 private:
  uint32_t const _definitionsCF;
  uint32_t const _documentsCF;
  uint32_t const _primaryCF;

  // these parameters are relevant to determine if we can print
  // a specific marker from the WAL
  TRI_vocbase_t* const _vocbase;
  // @brief collection replication UUID cache
  std::map<DataSourceId, CollectionGuard> _collectionCache;
  bool const _includeSystem;
  DataSourceId const _onlyDataSourceId;

  /// result builder
  VPackBuilder& _builder;

  // Various state machine flags
  rocksdb::SequenceNumber _startSequence;
  rocksdb::SequenceNumber _currentSequence;
  rocksdb::SequenceNumber _lastEmittedTick;  // just used for validation
  bool _startOfBatch = false;

  // Various state machine flags
  State _state = INVALID;
  TransactionId _currentTrxId = TransactionId::none();
  RevisionId _removedDocRid = RevisionId::none();
  std::string _oldCollectionName;
};

// iterates over WAL starting at 'from' and returns up to 'limit' documents
// from the corresponding database
RocksDBReplicationResult rocksutils::tailWal(TRI_vocbase_t* vocbase,
                                             uint64_t tickStart,
                                             uint64_t tickEnd, size_t chunkSize,
                                             bool includeSystem,
                                             DataSourceId collectionId,
                                             VPackBuilder& builder) {
  TRI_ASSERT(tickStart <= tickEnd);
  uint64_t lastTick = tickStart;         // generally contains begin of last wb
  uint64_t lastWrittenTick = tickStart;  // contains end tick of last wb
  uint64_t lastScannedTick = tickStart;

  // prevent purging of WAL files while we are in here
  auto& engine = vocbase->server()
                     .getFeature<EngineSelectorFeature>()
                     .engine<RocksDBEngine>();
  RocksDBFilePurgePreventer purgePreventer(engine.disallowPurging());

  // LOG_TOPIC("89157", WARN, Logger::FIXME) << "1. Starting tailing: tickStart
  // " << tickStart << " tickEnd " << tickEnd << " chunkSize " << chunkSize;//*/

  auto handler = std::make_unique<WALParser>(vocbase, includeSystem,
                                             collectionId, builder);

  // no need verifying the WAL contents
  rocksdb::TransactionLogIterator::ReadOptions ro(false);
  uint64_t since = 0;
  if (tickStart > 0) {
    since = tickStart - 1;
  }

  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;
  rocksdb::Status s = engine.db()->GetUpdatesSince(since, &iterator, ro);

  if (!s.ok()) {
    auto converted = convertStatus(s, rocksutils::StatusHint::wal);
    TRI_ASSERT(s.IsNotFound() || converted.fail());
    TRI_ASSERT(s.IsNotFound() || converted.errorNumber() != TRI_ERROR_NO_ERROR);
    if (s.IsNotFound()) {
      // specified from-tick not yet available in DB
      return {TRI_ERROR_NO_ERROR, 0};
    }
    return {converted.errorNumber(), lastTick};
  }

  bool minTickIncluded = false;
  // we need to check if the builder is bigger than the chunksize,
  // only after we printed a full WriteBatch. Otherwise a client might
  // never read the full writebatch
  while (iterator->Valid() && lastTick <= tickEnd &&
         builder.bufferRef().size() < chunkSize) {
    s = iterator->status();
    if (!s.ok()) {
      LOG_TOPIC("ed096", ERR, Logger::REPLICATION)
          << "error during WAL scan: " << s.ToString();
      break;  // s is considered in the end
    }

    rocksdb::BatchResult batch = iterator->GetBatch();
    TRI_ASSERT(lastTick == tickStart || batch.sequence >= lastTick);

    if (batch.sequence <= tickEnd) {
      lastScannedTick = batch.sequence;
    }

    if (!minTickIncluded && batch.sequence <= tickStart &&
        batch.sequence <= tickEnd) {
      minTickIncluded = true;
    }
    if (batch.sequence <= tickStart) {
      iterator->Next();  // skip
      continue;
    } else if (batch.sequence > tickEnd) {
      break;  // cancel out
    }

    lastTick = batch.sequence;
    LOG_TOPIC("5b4e9", TRACE, Logger::REPLICATION)
        << "Start WriteBatch tick: " << lastTick;
    handler->startNewBatch(batch.sequence);
    s = batch.writeBatchPtr->Iterate(handler.get());
    if (!s.ok()) {
      LOG_TOPIC("f4b88", ERR, Logger::REPLICATION)
          << "error during WAL scan: " << s.ToString();
      break;  // s is considered in the end
    }

    lastWrittenTick = handler->endBatch();
    LOG_TOPIC("024fc", TRACE, Logger::REPLICATION)
        << "End WriteBatch written-tick: " << lastWrittenTick;
    TRI_ASSERT(lastTick <= lastWrittenTick);
    if (!minTickIncluded && lastWrittenTick <= tickStart &&
        lastWrittenTick <= tickEnd) {
      minTickIncluded = true;
    }
    iterator->Next();
  }

  RocksDBReplicationResult result(TRI_ERROR_NO_ERROR, lastWrittenTick);
  result.lastScannedTick(lastScannedTick);
  if (!s.ok()) {  // TODO do something?
    result.reset(convertStatus(s, rocksutils::StatusHint::wal));
  }
  if (minTickIncluded) {
    result.includeMinTick();
  }

  TRI_ASSERT(!result.ok() || (result.maxTick() >= handler->lastEmittedTick()));
  // LOG_TOPIC("2f212", WARN, Logger::FIXME) << "2.  lastWrittenTick: " <<
  // lastWrittenTick;
  return result;
}
