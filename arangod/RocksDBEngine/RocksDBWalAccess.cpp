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

#include "RocksDBEngine/RocksDBWalAccess.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Replication/TailingSyncer.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBColumnFamilyManager.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBReplicationTailing.h"
#include "RocksDBEngine/RocksDBTypes.h"
#include "Utils/DatabaseGuard.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"

#include "Logger/Logger.h"

#include <rocksdb/utilities/transaction_db.h>
#include <velocypack/Builder.h>

using namespace arangodb;

RocksDBWalAccess::RocksDBWalAccess(RocksDBEngine& engine) : _engine(engine) {}

/// {"tickMin":"123", "tickMax":"456", "version":"3.2", "serverId":"abc"}
Result RocksDBWalAccess::tickRange(
    std::pair<TRI_voc_tick_t, TRI_voc_tick_t>& minMax) const {
  rocksdb::TransactionDB* tdb = _engine.db();
  rocksdb::VectorLogPtr walFiles;
  rocksdb::Status s = tdb->GetSortedWalFiles(walFiles);
  if (!s.ok()) {
    return rocksutils::convertStatus(s);
  }

  if (walFiles.size() > 0) {
    minMax.first = walFiles.front()->StartSequence();
  }
  minMax.second = tdb->GetLatestSequenceNumber();
  return TRI_ERROR_NO_ERROR;
}

/// {"lastTick":"123",
///  "version":"3.2",
///  "serverId":"abc",
///  "clients": {
///    "serverId": "ass", "lastTick":"123", ...
///  }}
///
TRI_voc_tick_t RocksDBWalAccess::lastTick() const {
  _engine.flushWal(false, false);
  return _engine.db()->GetLatestSequenceNumber();
}

/// WAL parser. Premise of this code is that transactions
/// can potentially be batched into the same rocksdb write batch
/// but transactions can never be interleaved with operations
/// outside of the transaction
class MyWALDumper final : public rocksdb::WriteBatch::Handler,
                          public WalAccessContext {
  // internal WAL parser states
  enum State : char {
    INVALID,
    DB_CREATE,
    DB_DROP,
    COLLECTION_CREATE,
    COLLECTION_RENAME,
    COLLECTION_CHANGE,
    INDEX_CREATE,
    INDEX_DROP,
    VIEW_CREATE,
    VIEW_CHANGE,
    TRANSACTION,
    SINGLE_PUT,
    SINGLE_REMOVE
  };

 public:
  MyWALDumper(RocksDBEngine& engine, WalAccess::Filter const& filter,
              WalAccess::MarkerCallback const& f, size_t maxResponseSize)
      : WalAccessContext(engine.server(), filter, f),
        _engine(engine),
        _definitionsCF(RocksDBColumnFamilyManager::get(
                           RocksDBColumnFamilyManager::Family::Definitions)
                           ->GetID()),
        _documentsCF(RocksDBColumnFamilyManager::get(
                         RocksDBColumnFamilyManager::Family::Documents)
                         ->GetID()),
        _primaryCF(RocksDBColumnFamilyManager::get(
                       RocksDBColumnFamilyManager::Family::PrimaryIndex)
                       ->GetID()),
        _maxResponseSize(maxResponseSize),
        _startSequence(0),
        _currentSequence(0),
        _lastWrittenSequence(0) {}

  bool Continue() override {
    if (_stopOnNext) {
      return false;
    }

    if (_responseSize > _maxResponseSize) {
      // it should only be possible to be in the middle of a huge batch,
      // if and only if we are in one big transaction. We may not stop
      if (_state == TRANSACTION && _removedDocRid.empty()) {
        // this will make us process one more marker still
        _stopOnNext = true;
      }
    }
    return true;
  }

  void LogData(rocksdb::Slice const& blob) override {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    _checkTick = true;
#endif
    // rocksdb does not count LogData towards sequence-number
    RocksDBLogType type = RocksDBLogValue::type(blob);

    // LOG_TOPIC("65ea8", WARN, Logger::REPLICATION) << "[LOG] " <<
    // _currentSequence << " " << rocksDBLogTypeName(type);
    switch (type) {
      case RocksDBLogType::DatabaseCreate:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleDB(RocksDBLogValue::databaseId(blob))) {
          _state = DB_CREATE;
        }
        // wait for marker data in Put entry
        break;
      case RocksDBLogType::DatabaseDrop:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleDB(RocksDBLogValue::databaseId(blob))) {
          _state = DB_DROP;
        }
        // wait for marker data in Put entry
        break;
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
          _state = COLLECTION_RENAME;  // collection name is not needed
          // LOG_TOPIC("3c411", DEBUG, Logger::REPLICATION) << "renaming "
          // << RocksDBLogValue::oldCollectionName(blob).toString();
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
        // always print drop collection marker, shouldHandleCollection will
        // always return false for dropped collections
        if (shouldHandleDB(dbid)) {
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            {  // tick number
              std::string_view uuid = RocksDBLogValue::viewUUID(blob);
              TRI_ASSERT(!uuid.empty());
              uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(tick)));
              marker->add("type", VPackValue(REPLICATION_COLLECTION_DROP));
              marker->add("db", VPackValue(vocbase->name()));
              marker->add("cuid", VPackValuePair(uuid.data(), uuid.size(),
                                                 VPackValueType::String));
            }
            printMarker(vocbase);
          }
        }
        break;
      }
      case RocksDBLogType::CollectionTruncate: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId cid = RocksDBLogValue::collectionId(blob);
        if (shouldHandleCollection(dbid, cid)) {  // will check vocbase
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            LogicalCollection* coll = loadCollection(dbid, cid);
            TRI_ASSERT(vocbase != nullptr && coll != nullptr);
            {
              uint64_t tick = _currentSequence;
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(tick)));
              marker->add("type", VPackValue(REPLICATION_COLLECTION_TRUNCATE));
              marker->add("db", VPackValue(vocbase->name()));
              marker->add("cuid", VPackValue(coll->guid()));
            }
            printMarker(vocbase);
          }
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
        // only print markers from this collection if it is set
        if (shouldHandleCollection(dbid, cid)) {  // will check vocbase
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            LogicalCollection* coll = loadCollection(dbid, cid);
            TRI_ASSERT(vocbase != nullptr && coll != nullptr);
            VPackSlice indexDef = RocksDBLogValue::indexSlice(blob);
            auto stripped = rocksutils::stripObjectIds(indexDef);

            {
              uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(tick)));
              marker->add("type", VPackValue(rocksutils::convertLogType(type)));
              marker->add("db", VPackValue(vocbase->name()));
              marker->add("cuid", VPackValue(coll->guid()));
              marker->add("data", stripped.first);
            }

            printMarker(vocbase);
          }
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
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            LogicalCollection* col = loadCollection(dbid, cid);
            TRI_ASSERT(vocbase != nullptr && col != nullptr);

            {
              uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(tick)));
              marker->add("type", VPackValue(rocksutils::convertLogType(type)));
              marker->add("db", VPackValue(vocbase->name()));
              marker->add("cuid", VPackValue(col->guid()));

              VPackObjectBuilder data(&_builder, "data", true);

              data->add("id", VPackValue(std::to_string(iid.id())));
            }

            printMarker(vocbase);
          }
        }
        break;
      }
      case RocksDBLogType::ViewCreate:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleView(RocksDBLogValue::databaseId(blob),
                             RocksDBLogValue::viewId(blob))) {
          _state = VIEW_CREATE;
        }
        // wait for marker data in Put entry
        break;

      case RocksDBLogType::ViewDrop: {
        resetTransientState();  // finish ongoing trx
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        DataSourceId vid = RocksDBLogValue::viewId(blob);
        if (shouldHandleView(dbid, vid)) {
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            {  // tick number
              std::string_view uuid = RocksDBLogValue::collectionUUID(blob);
              TRI_ASSERT(!uuid.empty());
              uint64_t tick = _currentSequence + (_startOfBatch ? 0 : 1);
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(tick)));
              marker->add("type", VPackValue(REPLICATION_VIEW_DROP));
              marker->add("db", VPackValue(vocbase->name()));
              marker->add("cuid", VPackValuePair(uuid.data(), uuid.size(),
                                                 VPackValueType::String));
            }

            printMarker(vocbase);
          }
        }
        // wait for marker data in Put entry
        break;
      }

      case RocksDBLogType::ViewChange:
        resetTransientState();  // finish ongoing trx
        if (shouldHandleView(RocksDBLogValue::databaseId(blob),
                             RocksDBLogValue::viewId(blob))) {
          _state = VIEW_CHANGE;
        }
        // wait for marker data in Put entry
        break;

      case RocksDBLogType::BeginTransaction: {
        resetTransientState();  // finish ongoing trx
        TransactionId tid = RocksDBLogValue::transactionId(blob);
        TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
        if (shouldHandleDB(dbid)) {
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          // note: vocbase may be a nullptr here, if the database was already
          // deleted!
          if (vocbase != nullptr) {
            _state = TRANSACTION;
            _currentTrxId = tid;
            _trxDbId = dbid;
            _builder.openObject(true);
            _builder.add("tick", VPackValue(std::to_string(_currentSequence)));
            _builder.add("type", VPackValue(rocksutils::convertLogType(type)));
            _builder.add("tid", VPackValue(std::to_string(_currentTrxId.id())));
            _builder.add("db", VPackValue(vocbase->name()));
            _builder.close();
            printMarker(vocbase);
          }
        }
        break;
      }
      case RocksDBLogType::CommitTransaction: {
        if (_state == TRANSACTION) {
          TRI_voc_tick_t dbid = RocksDBLogValue::databaseId(blob);
          TransactionId tid = RocksDBLogValue::transactionId(blob);
          TRI_ASSERT(_currentTrxId == tid && _trxDbId == dbid);
          if (shouldHandleDB(dbid) && _currentTrxId == tid) {
            writeCommitMarker(dbid);
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
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        _checkTick = false;
#endif
        break;  // ignore deprecated / unused markers

      default:
        LOG_TOPIC("0d0d1", WARN, Logger::REPLICATION)
            << "Unhandled wal log entry " << rocksDBLogTypeName(type);
        break;
    }
  }

  rocksdb::Status PutCF(uint32_t column_family_id, rocksdb::Slice const& key,
                        rocksdb::Slice const& value) override {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    _checkTick = true;
#endif
    incTick();
    // LOG_TOPIC("31697", WARN, Logger::ENGINES) << "[PUT] cf: " <<
    // column_family_id << ", key:" << key.ToString() << "  value: " <<
    // value.ToString();

    if (column_family_id == _definitionsCF) {
      // LogData should have triggered a commit on ongoing transactions
      if (RocksDBKey::type(key) == RocksDBEntryType::Database) {
        // database slice should contain "id" and "name"
        VPackSlice const data = RocksDBValue::data(value);
        VPackSlice const name = data.get("name");
        TRI_ASSERT(name.isString() && name.getStringLength() > 0);

        TRI_voc_tick_t dbid = RocksDBKey::databaseId(key);
        if (_state == DB_CREATE) {
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {  // db is already deleted
            {
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(_currentSequence)));
              marker->add("type", VPackValue(REPLICATION_DATABASE_CREATE));
              marker->add("db", name);
              marker->add("data", data);
            }
            printMarker(vocbase);
          }
        } else if (_state == DB_DROP) {
          // prepareDropDatabase should always write entry
          VPackSlice const del = data.get("deleted");
          TRI_ASSERT(del.isBool() && del.getBool());
          {
            VPackObjectBuilder marker(&_builder, true);
            marker->add("tick", VPackValue(std::to_string(_currentSequence)));
            marker->add("type", VPackValue(REPLICATION_DATABASE_DROP));
            marker->add("db", name);
          }
          printMarker(loadVocbase(dbid));
        }  // ignore Put in any other case
      } else if (RocksDBKey::type(key) == RocksDBEntryType::Collection) {
        TRI_voc_tick_t dbid = RocksDBKey::databaseId(key);
        DataSourceId cid = RocksDBKey::collectionId(key);

        if (shouldHandleCollection(dbid, cid) &&
            (_state == COLLECTION_CREATE || _state == COLLECTION_RENAME ||
             _state == COLLECTION_CHANGE)) {
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            LogicalCollection* col = loadCollection(dbid, cid);
            TRI_ASSERT(vocbase != nullptr && col != nullptr);

            {
              VPackSlice collectionDef = RocksDBValue::data(value);
              VPackObjectBuilder marker(&_builder, true);
              marker->add("tick", VPackValue(std::to_string(_currentSequence)));
              marker->add("db", VPackValue(vocbase->name()));
              marker->add("cuid", VPackValue(col->guid()));

              if (_state == COLLECTION_CREATE) {
                auto stripped = rocksutils::stripObjectIds(collectionDef);
                marker->add("type", VPackValue(REPLICATION_COLLECTION_CREATE));
                marker->add("data", stripped.first);
              } else if (_state == COLLECTION_RENAME) {
                marker->add("type", VPackValue(REPLICATION_COLLECTION_RENAME));
                VPackObjectBuilder data(&_builder, "data", true);
                data->add("name", VPackValue(col->name()));
              } else if (_state == COLLECTION_CHANGE) {
                auto stripped = rocksutils::stripObjectIds(collectionDef);
                marker->add("type", VPackValue(REPLICATION_COLLECTION_CHANGE));
                marker->add("data", stripped.first);
              }
            }
            printMarker(vocbase);
          }
        }
      } else if (RocksDBKey::type(key) == RocksDBEntryType::View) {
        TRI_voc_tick_t dbid = RocksDBKey::databaseId(key);
        DataSourceId vid = RocksDBKey::viewId(key);

        if (shouldHandleView(dbid, vid) &&
            (_state == VIEW_CREATE || _state == VIEW_CHANGE)) {
          TRI_vocbase_t* vocbase = loadVocbase(dbid);
          if (vocbase != nullptr) {
            auto view = vocbase->lookupView(vid);

            if (view != nullptr) {  // ignore nonexisting views
              VPackSlice viewDef = RocksDBValue::data(value);
              {
                VPackObjectBuilder marker(&_builder, true);
                marker->add("tick",
                            VPackValue(std::to_string(_currentSequence)));
                marker->add("db", VPackValue(vocbase->name()));
                marker->add("cuid", VPackValue(view->guid()));
                marker->add("data", viewDef);
                if (_state == VIEW_CREATE) {
                  marker->add("type", VPackValue(REPLICATION_VIEW_CREATE));
                } else /*if (_state == VIEW_CHANGE)*/ {
                  marker->add("type", VPackValue(REPLICATION_VIEW_CHANGE));
                }
              }
              printMarker(vocbase);
            }
          }
        }
      }

      // reset everything immediately after DDL operations
      resetTransientState();

    } else if (column_family_id == _documentsCF) {
      if (_state != TRANSACTION && _state != SINGLE_PUT) {
        resetTransientState();
        return rocksdb::Status();
      }
      TRI_ASSERT(_state != SINGLE_PUT || _currentTrxId.empty());
      TRI_ASSERT(_state != TRANSACTION || _trxDbId != 0);
      TRI_ASSERT(_removedDocRid.empty());
      _removedDocRid = RevisionId::none();

      uint64_t objectId = RocksDBKey::objectId(key);
      auto dbCollPair = _engine.mapObjectToCollection(objectId);
      TRI_voc_tick_t const dbid = dbCollPair.first;
      DataSourceId const cid = dbCollPair.second;

      if (!shouldHandleCollection(dbid, cid)) {
        return rocksdb::Status();  // no reset here
      }

      TRI_ASSERT(_state != TRANSACTION || _trxDbId == dbid);

      TRI_vocbase_t* vocbase = loadVocbase(dbid);
      LogicalCollection* col = loadCollection(dbid, cid);
      TRI_ASSERT(vocbase != nullptr && col != nullptr);

      {
        VPackObjectBuilder marker(&_builder, true);
        marker->add("tick", VPackValue(std::to_string(_currentSequence)));
        marker->add("type", VPackValue(REPLICATION_MARKER_DOCUMENT));
        marker->add("db", VPackValue(vocbase->name()));
        marker->add("cuid", VPackValue(col->guid()));
        marker->add("tid", VPackValue(std::to_string(_currentTrxId.id())));
        marker->add("data", RocksDBValue::data(value));
      }

      printMarker(vocbase);
      if (_state == SINGLE_PUT) {
        resetTransientState();  // always reset after single op
      }
    }

    return rocksdb::Status();
  }

  // for Delete / SingleDelete
  void handleDeleteCF(uint32_t cfId, rocksdb::Slice const& key) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    _checkTick = true;
#endif
    incTick();
    // LOG_TOPIC("5060c", WARN, Logger::ENGINES) << "[DELETE] cf: " << cfId <<
    // ", key:" << key.ToString();

    if (cfId != _primaryCF) {
      return;  // ignore all document operations
    }

    if (_state != TRANSACTION && _state != SINGLE_REMOVE) {
      resetTransientState();
      return;
    }
    TRI_ASSERT(_state != SINGLE_REMOVE || _currentTrxId.empty());
    TRI_ASSERT(_state != TRANSACTION || _trxDbId != 0);

    uint64_t objectId = RocksDBKey::objectId(key);
    auto triple = _engine.mapObjectToIndex(objectId);
    TRI_voc_tick_t const dbid = std::get<0>(triple);
    DataSourceId const cid = std::get<1>(triple);

    if (!shouldHandleCollection(dbid, cid)) {
      _removedDocRid = RevisionId::none();  // ignore rid too
      return;                               // no reset here
    }

    std::string_view docKey = RocksDBKey::primaryKey(key);
    TRI_ASSERT(_state != TRANSACTION || _trxDbId == dbid);

    TRI_vocbase_t* vocbase = loadVocbase(dbid);
    LogicalCollection* col = loadCollection(dbid, cid);
    TRI_ASSERT(vocbase != nullptr && col != nullptr);

    {
      VPackObjectBuilder marker(&_builder, true);
      marker->add("tick", VPackValue(std::to_string(_currentSequence)));
      marker->add("type", VPackValue(REPLICATION_MARKER_REMOVE));
      marker->add("db", VPackValue(vocbase->name()));
      marker->add("cuid", VPackValue(col->guid()));
      marker->add("tid", VPackValue(std::to_string(_currentTrxId.id())));

      VPackObjectBuilder data(&_builder, "data", true);
      data->add(
          StaticStrings::KeyString,
          VPackValuePair(docKey.data(), docKey.size(), VPackValueType::String));
      data->add(StaticStrings::RevString,
                VPackValue(_removedDocRid.toString()));
    }

    printMarker(vocbase);
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
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    _checkTick = true;
#endif
    incTick();
    // LOG_TOPIC("d1dfa", WARN, Logger::ENGINES) << "[DELETE-RANGE] cf: " <<
    // column_family_id; drop and truncate may use this, but we do not print
    // anything
    return rocksdb::Status();  // make WAL iterator happy
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

  rocksdb::Status MarkNoop(bool /*empty_batch*/) override {
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

 public:
  /// figures out from which sequence number we need to start scanning
  /// if we just use tickStart rocksdb will skip over batches we might
  /// not have completely evaluated
  uint64_t safeBeginTick() const {
    if (_filter.tickLastScanned > 0 &&
        _filter.tickLastScanned < _filter.tickStart) {
      return _filter.tickLastScanned;
    }
    return _filter.tickStart;
  }

  void startNewBatch(rocksdb::SequenceNumber startSequence) {
    // LOG_TOPIC("24e69", TRACE, Logger::ENGINES) << "starting new batch with
    // sequence: " << startSequence;
    TRI_ASSERT(!_stopOnNext);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    if (_checkTick) {
      TRI_ASSERT(_startSequence < startSequence);
      TRI_ASSERT(_currentSequence < startSequence);
    }
#endif

    // starting new write batch
    _startSequence = startSequence;
    _currentSequence = startSequence;
    _startOfBatch = true;
    _state = INVALID;
    _currentTrxId = TransactionId::none();
    _trxDbId = 0;
    _removedDocRid = RevisionId::none();
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    _checkTick = true;
#endif
  }

  uint64_t endBatch() {
    TRI_ASSERT(_removedDocRid.empty() || _stopOnNext);
    resetTransientState();
    return _currentSequence;
  }

  size_t responseSize() const { return _responseSize; }

  uint64_t lastWrittenSequence() const { return _lastWrittenSequence; }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  void disableTickCheck() { _checkTick = false; }
#endif

 private:
  void writeCommitMarker(TRI_voc_tick_t dbid) {
    TRI_ASSERT(_state == TRANSACTION);
    TRI_vocbase_t* vocbase = loadVocbase(dbid);
    // note: vocbase may be a nullptr here, if the database was already deleted!
    if (vocbase != nullptr) {
      _builder.openObject(true);
      _builder.add("tick", VPackValue(std::to_string(_currentSequence)));
      _builder.add(
          "type",
          VPackValue(static_cast<uint64_t>(REPLICATION_TRANSACTION_COMMIT)));
      _builder.add("tid", VPackValue(std::to_string(_currentTrxId.id())));
      _builder.add("db", VPackValue(vocbase->name()));
      _builder.close();

      printMarker(vocbase);
    }
    _state = INVALID;
  }

  /// print maker in builder and clear it
  void printMarker(TRI_vocbase_t* vocbase) {
    // note: vocbase may be a nullptr here!
    TRI_ASSERT(!_builder.isEmpty());
    if (_currentSequence > _filter.tickStart) {
      _callback(vocbase, _builder.slice());
      _responseSize += _builder.size();
      _lastWrittenSequence = _currentSequence;
    }
    _builder.clear();
  }

  // should reset state flags which are only valid between
  // observing a specific log entry and a sequence of immediately
  // following PUT / DELETE / Log entries
  void resetTransientState() {
    // reset all states
    _state = INVALID;
    _currentTrxId = TransactionId::none();
    _trxDbId = 0;
    _removedDocRid = RevisionId::none();
  }

  // tick function that is called before each new WAL entry
  void incTick() {
    if (_startOfBatch) {
      // we are at the start of a batch. do NOT increase sequence number
      _startOfBatch = false;
    } else {
      // we are inside a batch already. now increase sequence number
      ++_currentSequence;
    }
  }

 private:
  RocksDBEngine& _engine;
  uint32_t const _definitionsCF;
  uint32_t const _documentsCF;
  uint32_t const _primaryCF;
  size_t const _maxResponseSize;

  rocksdb::SequenceNumber _startSequence;
  rocksdb::SequenceNumber _currentSequence;
  rocksdb::SequenceNumber _lastWrittenSequence;
  bool _startOfBatch = false;

  // Various state machine flags
  State _state = INVALID;
  TransactionId _currentTrxId = TransactionId::none();
  TRI_voc_tick_t _trxDbId = 0;  // remove eventually
  RevisionId _removedDocRid = RevisionId::none();
  bool _stopOnNext = false;
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  bool _checkTick = true;
#endif
};

/// @brief helper function to print WAL contents. this is only used for
/// debugging
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
void RocksDBWalAccess::printWal(Filter const& filter, size_t chunkSize,
                                MarkerCallback const& func) const {
  rocksdb::TransactionDB* db = _engine.db();

  if (chunkSize < 16384) {  // we need to have some sensible minimum
    chunkSize = 16384;
  }

  // pre 3.4 breaking up write batches is not supported
  size_t maxTrxChunkSize = filter.tickLastScanned > 0 ? chunkSize : SIZE_MAX;

  MyWALDumper dumper(_engine, filter, func, maxTrxChunkSize);
  const uint64_t since = dumper.safeBeginTick();
  TRI_ASSERT(since <= filter.tickStart);
  TRI_ASSERT(since <= filter.tickEnd);

  uint64_t firstTick = UINT64_MAX;   // first tick to actually print (exclusive)
  uint64_t lastScannedTick = since;  // last (begin) tick of batch we looked at
  uint64_t latestTick = db->GetLatestSequenceNumber();

  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;
  rocksdb::TransactionLogIterator::ReadOptions ro(false);
  rocksdb::Status s = db->GetUpdatesSince(since, &iterator, ro);
  TRI_ASSERT(s.ok());

  // we need to check if the builder is bigger than the chunksize,
  // only after we printed a full WriteBatch. Otherwise a client might
  // never read the full writebatch
  LOG_TOPIC("caefa", WARN, Logger::ENGINES)
      << "WAL tailing call. Scan since: " << since
      << ", tick start: " << filter.tickStart
      << ", tick end: " << filter.tickEnd << ", chunk size: " << chunkSize
      << ", latesttick: " << latestTick;
  while (iterator->Valid() && lastScannedTick <= filter.tickEnd) {
    rocksdb::BatchResult batch = iterator->GetBatch();
    // record the first tick we are actually considering
    if (firstTick == UINT64_MAX) {
      firstTick = batch.sequence;
    }

    if (batch.sequence > filter.tickEnd) {
      break;  // cancel out
    }

    LOG_TOPIC("a9d9c", WARN, Logger::REPLICATION)
        << "found batch-seq: " << batch.sequence
        << ", count: " << batch.writeBatchPtr->Count()
        << ", last scanned: " << lastScannedTick;
    lastScannedTick = batch.sequence;  // start of the batch
    TRI_ASSERT(lastScannedTick <= db->GetLatestSequenceNumber());

    if (batch.sequence < since) {
      iterator->Next();  // skip
      continue;
    }

    dumper.startNewBatch(batch.sequence);
    s = batch.writeBatchPtr->Iterate(&dumper);
    if (batch.writeBatchPtr->Count() == 0) {
      // there can be completely empty write batches. in case we encounter
      // some, we cannot assume the tick gets increased next time
      dumper.disableTickCheck();
    }
    TRI_ASSERT(s.ok());

    uint64_t batchEndSeq = dumper.endBatch();  // end tick of the batch
    TRI_ASSERT(batchEndSeq >= lastScannedTick);

    if (dumper.responseSize() >= chunkSize) {  // break if response gets big
      LOG_TOPIC("205d3", WARN, Logger::REPLICATION)
          << "reached maximum result size. finishing tailing";
      break;
    }
    // we need to set this here again, to avoid re-scanning WriteBatches
    lastScannedTick = batchEndSeq;  // do not remove, tailing take forever
    TRI_ASSERT(lastScannedTick <= db->GetLatestSequenceNumber());

    iterator->Next();
  }

  latestTick = db->GetLatestSequenceNumber();

  LOG_TOPIC("5b5a1", WARN, Logger::REPLICATION)
      << "lastScannedTick: " << lastScannedTick
      << ", latestTick: " << latestTick;
}
#endif

// iterates over WAL starting at 'from' and returns up to 'chunkSize' documents
// from the corresponding database
WalAccessResult RocksDBWalAccess::tail(Filter const& filter, size_t chunkSize,
                                       MarkerCallback const& func) const {
  TRI_ASSERT(filter.transactionIds.empty());  // not supported in any way

  rocksdb::TransactionDB* db = _engine.db();

  if (chunkSize < 16384) {  // we need to have some sensible minimum
    chunkSize = 16384;
  }

  // pre 3.4 breaking up write batches is not supported
  size_t maxTrxChunkSize = filter.tickLastScanned > 0 ? chunkSize : SIZE_MAX;

  MyWALDumper dumper(_engine, filter, func, maxTrxChunkSize);
  const uint64_t since = dumper.safeBeginTick();
  TRI_ASSERT(since <= filter.tickStart);
  TRI_ASSERT(since <= filter.tickEnd);

  uint64_t firstTick = UINT64_MAX;   // first tick to actually print (exclusive)
  uint64_t lastScannedTick = since;  // last (begin) tick of batch we looked at
  uint64_t lastWrittenTick = 0;      // lastTick at the end of a write batch
  uint64_t latestTick = db->GetLatestSequenceNumber();

  // prevent purging of WAL files while we are in here
  RocksDBFilePurgePreventer purgePreventer(_engine.disallowPurging());

  std::unique_ptr<rocksdb::TransactionLogIterator> iterator;
  // no need verifying the WAL contents
  rocksdb::TransactionLogIterator::ReadOptions ro(false);
  rocksdb::Status s = db->GetUpdatesSince(since, &iterator, ro);
  if (!s.ok()) {
    Result r = convertStatus(s, rocksutils::StatusHint::wal);
    return WalAccessResult(r.errorNumber(), filter.tickStart == latestTick, 0,
                           /*lastScannedTick*/ 0, latestTick);
  }

  // we need to check if the builder is bigger than the chunksize,
  // only after we printed a full WriteBatch. Otherwise a client might
  // never read the full writebatch
  LOG_TOPIC("caefb", DEBUG, Logger::ENGINES)
      << "WAL tailing call. Scan since: " << since
      << ", tick start: " << filter.tickStart
      << ", tick end: " << filter.tickEnd << ", chunk size: " << chunkSize;
  while (iterator->Valid() && lastScannedTick <= filter.tickEnd) {
    rocksdb::BatchResult batch = iterator->GetBatch();
    // record the first tick we are actually considering
    if (firstTick == UINT64_MAX) {
      firstTick = batch.sequence;
    }

    if (batch.sequence > filter.tickEnd) {
      break;  // cancel out
    }

    // LOG_TOPIC("1eccb", TRACE, Logger::REPLICATION) << "found batch-seq: " <<
    // batch.sequence;
    lastScannedTick = batch.sequence;  // start of the batch
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    if (lastScannedTick > db->GetLatestSequenceNumber()) {
      // this is an unexpected condition. in this case we print the WAL for
      // debug purposes and error out
      printWal(filter, chunkSize, func);
    }
#endif
    TRI_ASSERT(lastScannedTick <= db->GetLatestSequenceNumber());

    if (batch.sequence < since) {
      // LOG_TOPIC("a5e56", TRACE, Logger::REPLICATION) << "skipping batch from
      // " << batch.sequence << " to " << (batch.sequence +
      // batch.writeBatchPtr->Count());
      iterator->Next();  // skip
      continue;
    }

    dumper.startNewBatch(batch.sequence);
    s = batch.writeBatchPtr->Iterate(&dumper);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    if (batch.writeBatchPtr->Count() == 0) {
      // there can be completely empty write batches. in case we encounter
      // some, we cannot assume the tick gets increased next time
      dumper.disableTickCheck();
    }
#endif
    if (!s.ok()) {
      LOG_TOPIC("57d54", ERR, Logger::REPLICATION)
          << "error during WAL scan: " << s.ToString();
      break;  // s is considered in the end
    }

    uint64_t batchEndSeq = dumper.endBatch();  // end tick of the batch
    lastWrittenTick =
        dumper.lastWrittenSequence();  // 0 if no marker was written
    TRI_ASSERT(batchEndSeq >= lastScannedTick);

    if (dumper.responseSize() >= chunkSize) {  // break if response gets big
      break;
    }
    // we need to set this here again, to avoid re-scanning WriteBatches
    lastScannedTick = batchEndSeq;  // do not remove, tailing take forever
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    if (lastScannedTick > db->GetLatestSequenceNumber()) {
      // this is an unexpected condition. in this case we print the WAL for
      // debug purposes and error out
      printWal(filter, chunkSize, func);
    }
#endif
    TRI_ASSERT(lastScannedTick <= db->GetLatestSequenceNumber());

    iterator->Next();
  }

  // update our latest sequence number again, because it may have been raised
  // while scanning the WAL
  latestTick = db->GetLatestSequenceNumber();

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (s.ok() && lastScannedTick > latestTick) {
    // this is an unexpected condition. in this case we print the WAL for
    // debug purposes and error out
    printWal(filter, chunkSize, func);
  }
#endif

  TRI_ASSERT(!s.ok() || lastScannedTick <= latestTick);

  WalAccessResult result(TRI_ERROR_NO_ERROR, firstTick <= filter.tickStart,
                         lastWrittenTick, lastScannedTick, latestTick);
  if (!s.ok()) {
    result.reset(convertStatus(s, rocksutils::StatusHint::wal));
  }
  // LOG_TOPIC("f7ab7", WARN, Logger::REPLICATION) << "2. firstTick: " <<
  // firstTick << " lastWrittenTick: " << lastWrittenTick
  // << " latestTick: " << latestTick;
  return result;
}
