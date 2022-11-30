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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Syncer.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ConditionLocker.h"
#include "Basics/Exceptions.h"
#include "Basics/MutexLocker.h"
#include "Basics/RocksDBUtils.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Rest/HttpRequest.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "SimpleHttpClient/GeneralClientConnection.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionGuard.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/Identifiers/LocalDocumentId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/Methods/Indexes.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Slice.h>

namespace {

constexpr std::string_view cuidRef("cuid");
constexpr std::string_view dbRef("db");
constexpr std::string_view databaseRef("database");
constexpr std::string_view globallyUniqueIdRef("globallyUniqueId");

/// @brief extract the collection id from VelocyPack
arangodb::DataSourceId getCid(arangodb::velocypack::Slice const& slice) {
  return arangodb::DataSourceId{
      arangodb::basics::VelocyPackHelper::extractIdValue(slice)};
}

/// @brief extract the collection name from VelocyPack
std::string getCName(arangodb::velocypack::Slice const& slice) {
  return arangodb::basics::VelocyPackHelper::getStringValue(slice, "cname", "");
}

/// @brief extract the collection by either id or name, may return nullptr!
std::shared_ptr<arangodb::LogicalCollection> getCollectionByIdOrName(
    TRI_vocbase_t& vocbase, arangodb::DataSourceId cid,
    std::string const& name) {
  auto idCol = vocbase.lookupCollection(cid);
  std::shared_ptr<arangodb::LogicalCollection> nameCol;

  if (!name.empty()) {
    // try looking up the collection by name then
    nameCol = vocbase.lookupCollection(name);
  }

  if (idCol != nullptr && nameCol != nullptr) {
    if (idCol->id() == nameCol->id()) {
      // found collection by id and name, and both are identical!
      return idCol;
    }

    // found different collections by id and name
    TRI_ASSERT(!name.empty());

    if (name[0] == '_') {
      // system collection. always return collection by name when in doubt
      return nameCol;
    }

    // no system collection. still prefer local collection
    return nameCol;
  }

  if (nameCol != nullptr) {
    TRI_ASSERT(idCol == nullptr);
    return nameCol;
  }

  // may be nullptr
  return idCol;
}

/// @brief apply the data from a collection dump or the continuous log
arangodb::Result applyCollectionDumpMarkerInternal(
    arangodb::Syncer::SyncerState const& state,
    arangodb::transaction::Methods& trx, arangodb::LogicalCollection* coll,
    arangodb::TRI_replication_operation_e type, VPackSlice const& slice,
    std::string& conflictingDocumentKey) {
  using arangodb::OperationOptions;
  using arangodb::OperationResult;
  using arangodb::Result;

  // key must not be empty
  auto const keySlice = slice.get(arangodb::StaticStrings::KeyString);
  if (!keySlice.isString() || keySlice.getStringLength() == 0) {
    return TRI_ERROR_REPLICATION_INVALID_RESPONSE;
  }

  if (type ==
      arangodb::TRI_replication_operation_e::REPLICATION_MARKER_DOCUMENT) {
    // {"type":2300,"key":"230274209405676","data":{"_key":"230274209405676","_rev":"230274209405676","foo":"bar"}}

    OperationOptions options;
    options.silent = true;
    options.ignoreRevs = true;
    options.isRestore = true;
    if (!state.leaderId.empty()) {
      options.isSynchronousReplicationFrom = state.leaderId;
    }
    // we want the conflicting other key returned in case of unique constraint
    // violation
    options.indexOperationMode = arangodb::IndexOperationMode::internal;

    try {
      OperationResult opRes(Result(), options);
      auto potentiallyConflictingKey = std::string{};
      bool useReplace = false;

      // if we are about to process a single document marker we first check if
      // the target document exists. if yes, we don't try an insert (which would
      // fail anyway) but carry on with a replace.
      std::pair<arangodb::LocalDocumentId, arangodb::RevisionId> lookupResult;
      // We must see our own writes, because we may have to remove conflicting
      // documents (that we just inserted) as documents may be replicated in
      // unexpected order.
      if (coll->getPhysical()
              ->lookupKey(&trx, keySlice.stringView(), lookupResult,
                          arangodb::ReadOwnWrites::yes)
              .ok()) {
        // determine if we already have this revision or need to replace the
        // one we have
        arangodb::RevisionId rid = arangodb::RevisionId::fromSlice(slice);
        if (rid.isSet() && rid == lookupResult.second) {
          // we already have exactly this document, don't replace, just
          // consider it already applied and bail
          return {};
        }

        // need to replace the one we have
        useReplace = true;
        potentiallyConflictingKey = keySlice.copyString();
      }

      if (!useReplace) {
        // try insert first
        opRes = trx.insert(coll->name(), slice, options);
        if (opRes.is(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED)) {
          useReplace = true;
          // In this case (and this case only), the errorMessage contains the
          // conflicting document's key.
          potentiallyConflictingKey = opRes.errorMessage();
        } else {
          potentiallyConflictingKey.clear();
        }
      }

      if (useReplace) {
        if (keySlice.stringView() != potentiallyConflictingKey) {
          // different key
          if (trx.isSingleOperationTransaction()) {
            // return the conflicting document's key to retry
            conflictingDocumentKey = potentiallyConflictingKey;
            return Result(TRI_ERROR_ARANGO_TRY_AGAIN);
          }

          VPackBuilder tmp;
          tmp.add(VPackValue(opRes.errorMessage()));

          opRes = trx.remove(coll->name(), tmp.slice(), options);
          if (opRes.ok() || !opRes.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
            useReplace = false;
          }
        }

        int tries = 0;
        while (tries++ < 2) {
          if (useReplace) {
            // perform a replace
            opRes = trx.replace(coll->name(), slice, options);
          } else {
            // perform a re-insert
            opRes = trx.insert(coll->name(), slice, options);
          }

          if (opRes.ok() ||
              !opRes.is(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED) ||
              trx.isSingleOperationTransaction()) {
            break;
          }

          // in case we get a unique constraint violation in a multi-document
          // transaction, we can remove the conflicting document and try again
          options.indexOperationMode = arangodb::IndexOperationMode::normal;

          VPackBuilder tmp;
          tmp.add(VPackValue(opRes.errorMessage()));

          trx.remove(coll->name(), tmp.slice(), options);
        }
      }

      return std::move(opRes.result);
    } catch (arangodb::basics::Exception const& ex) {
      return Result(ex.code(),
                    std::string("document insert/replace operation failed: ") +
                        ex.what());
    } catch (std::exception const& ex) {
      return Result(TRI_ERROR_INTERNAL,
                    std::string("document insert/replace operation failed: ") +
                        ex.what());
    } catch (...) {
      return Result(
          TRI_ERROR_INTERNAL,
          std::string(
              "document insert/replace operation failed: unknown exception"));
    }
  } else if (type ==
             arangodb::TRI_replication_operation_e::REPLICATION_MARKER_REMOVE) {
    // {"type":2302,"key":"592063"}

    try {
      OperationOptions options;
      options.silent = true;
      options.ignoreRevs = true;
      if (!state.leaderId.empty()) {
        options.isSynchronousReplicationFrom = state.leaderId;
      }
      OperationResult opRes = trx.remove(coll->name(), slice, options);

      if (opRes.ok() || opRes.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND)) {
        // ignore document not found errors
        return Result();
      }

      return Result(opRes.result);
    } catch (arangodb::basics::Exception const& ex) {
      return Result(
          ex.code(),
          std::string("document remove operation failed: ") + ex.what());
    } catch (std::exception const& ex) {
      return Result(
          TRI_ERROR_INTERNAL,
          std::string("document remove operation failed: ") + ex.what());
    } catch (...) {
      return Result(
          TRI_ERROR_INTERNAL,
          std::string("document remove operation failed: unknown exception"));
    }
  }

  return Result(TRI_ERROR_REPLICATION_UNEXPECTED_MARKER,
                std::string("unexpected marker type ") +
                    arangodb::basics::StringUtils::itoa(type));
}

}  // namespace

namespace arangodb {

Syncer::JobSynchronizer::JobSynchronizer(
    std::shared_ptr<Syncer const> const& syncer)
    : _syncer(syncer), _gotResponse(false), _time(0.0), _jobsInFlight(0) {}

Syncer::JobSynchronizer::~JobSynchronizer() {
  // signal that we have got something
  try {
    gotResponse(Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED));
  } catch (...) {
    // must not throw from here
  }

  // wait until all posted jobs have been completed/canceled
  while (hasJobInFlight()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::yield();
  }
}

bool Syncer::JobSynchronizer::gotResponse() const noexcept {
  CONDITION_LOCKER(guard, _condition);
  return _gotResponse;
}

/// @brief will be called whenever a response for the job comes in
void Syncer::JobSynchronizer::gotResponse(
    std::unique_ptr<arangodb::httpclient::SimpleHttpResult> response,
    double time) noexcept {
  CONDITION_LOCKER(guard, _condition);
  _res.reset();  // no error!
  _response = std::move(response);
  _gotResponse = true;
  _time = time;

  guard.signal();
}

/// @brief will be called whenever an error occurred
/// expects "res" to be an error!
void Syncer::JobSynchronizer::gotResponse(arangodb::Result&& res,
                                          double time) noexcept {
  TRI_ASSERT(res.fail());

  CONDITION_LOCKER(guard, _condition);
  _res = std::move(res);
  _response.reset();
  _gotResponse = true;
  _time = time;

  guard.signal();
}

/// @brief the calling Syncer will call and block inside this function until
/// there is a response or the syncer/server is shut down
Result Syncer::JobSynchronizer::waitForResponse(
    std::unique_ptr<arangodb::httpclient::SimpleHttpResult>& response) {
  while (true) {
    {
      CONDITION_LOCKER(guard, _condition);

      if (!_gotResponse) {
        guard.wait(1 * 1000 * 1000);
      }

      // check again, _gotResponse may have changed
      if (_gotResponse) {
        _gotResponse = false;
        response = std::move(_response);
        return _res;
      }
    }

    if (_syncer->isAborted()) {
      // clear result response
      response.reset();

      CONDITION_LOCKER(guard, _condition);
      _gotResponse = false;
      _response.reset();
      _res.reset();

      // will result in returning TRI_ERROR_REPLICATION_APPLIER_STOPPED
      break;
    }
  }

  return Result(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
}

void Syncer::JobSynchronizer::request(std::function<void()> const& cb) {
  // by indicating that we have posted an async job, the caller
  // will block on exit until all posted jobs have finished
  if (!jobPosted()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_REPLICATION_APPLIER_STOPPED);
  }

  SchedulerFeature::SCHEDULER->queue(
      RequestLane::INTERNAL_LOW, [self = shared_from_this(), cb]() {
        // whatever happens next, when we leave this here, we need to indicate
        // that there is no more posted job.
        // otherwise the calling thread may block forever waiting on the
        // posted jobs to finish
        auto guard = scopeGuard([self]() noexcept { self->jobDone(); });

        cb();
      });
}

/// @brief notifies that a job was posted
/// returns false if job counter could not be increased (e.g. because
/// the syncer was stopped/aborted already)
bool Syncer::JobSynchronizer::jobPosted() {
  while (true) {
    CONDITION_LOCKER(guard, _condition);

    // _jobsInFlight should be 0 in almost all cases, however, there
    // is a small window in which the request has been processed already
    // (i.e. after waitForResponse() has returned and before jobDone()
    // has been called and has decreased _jobsInFlight). For this
    // particular case, we simply wait for _jobsInFlight to become 0 again
    if (_jobsInFlight == 0) {
      ++_jobsInFlight;
      return true;
    }

    if (_syncer->isAborted()) {
      // syncer already stopped... no need to carry on here
      return false;
    }
    guard.wait(10 * 1000);
  }
}

/// @brief notifies that a job was done
void Syncer::JobSynchronizer::jobDone() noexcept {
  CONDITION_LOCKER(guard, _condition);

  TRI_ASSERT(_jobsInFlight == 1);
  --_jobsInFlight;
  _condition.signal();
}

/// @brief checks if there are jobs in flight (can be 0 or 1 job only)
bool Syncer::JobSynchronizer::hasJobInFlight() const noexcept {
  CONDITION_LOCKER(guard, _condition);

  TRI_ASSERT(_jobsInFlight <= 1);
  return _jobsInFlight > 0;
}

/**
 * @brief Generate a new syncer ID, used for the catchup in synchronous
 * replication.
 *
 * If we're running in a cluster, we're a DBServer that's using asynchronous
 * replication to catch up until we can switch to synchronous replication.
 *
 * As in this case multiple syncers can run on the same client, syncing from the
 * same server, the server ID used to identify the client with usual
 * asynchronous replication on the server is not sufficiently unique. For that
 * case, we use the syncer ID with a server specific tick.
 *
 * Otherwise, we're doing some other kind of asynchronous replication (e.g.
 * active failover or dc2dc). In that case, the server specific tick would not
 * be unique among clients, and the server ID will be used instead.
 *
 * The server distinguishes between syncer and server IDs, which is why we don't
 * just return ServerIdFeature::getId() here, so e.g. SyncerId{4} and server ID
 * 4 will be handled as distinct values.
 */
SyncerId newSyncerId() {
  if (ServerState::instance()->isRunningInCluster()) {
    TRI_ASSERT(ServerState::instance()->getShortId() != 0);
    return SyncerId{TRI_NewServerSpecificTick()};
  }

  return SyncerId{0};
}

Syncer::SyncerState::SyncerState(
    Syncer* syncer, ReplicationApplierConfiguration const& configuration)
    : syncerId{newSyncerId()},
      applier{configuration},
      connection{syncer, configuration},
      leader{configuration} {}

Syncer::Syncer(ReplicationApplierConfiguration const& configuration)
    : _state{this, configuration} {
  if (!ServerState::instance()->isSingleServer() &&
      !ServerState::instance()->isDBServer()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_NOT_IMPLEMENTED,
        "the replication functionality is supposed to be invoked only on a "
        "single server or DB server");
  }
  if (!_state.applier._database.empty()) {
    // use name from configuration
    _state.databaseName = _state.applier._database;
  }

  if (_state.applier._chunkSize == 0) {
    _state.applier._chunkSize = 2 * 1024 * 1024;  // default: 2 MB
  } else if (_state.applier._chunkSize < 16 * 1024) {
    _state.applier._chunkSize = 16 * 1024;
  }

  // get our own server-id
  _state.localServerId = ServerIdFeature::getId();
  _state.localServerIdString =
      basics::StringUtils::itoa(_state.localServerId.id());

  _state.leader.endpoint = _state.applier._endpoint;
}

Syncer::~Syncer() = default;

/// @brief request location rewriter (injects database name)
std::string Syncer::rewriteLocation(void* data, std::string const& location) {
  Syncer* s = static_cast<Syncer*>(data);
  TRI_ASSERT(s != nullptr);
  if (location.starts_with("/_db/")) {
    // location already contains /_db/
    return location;
  }
  TRI_ASSERT(!s->_state.databaseName.empty());
  if (location[0] == '/') {
    return "/_db/" + basics::StringUtils::urlEncode(s->_state.databaseName) +
           location;
  }
  return "/_db/" + basics::StringUtils::urlEncode(s->_state.databaseName) +
         "/" + location;
}

void Syncer::setAborted(bool value) { _state.connection.setAborted(value); }

bool Syncer::isAborted() const { return _state.connection.isAborted(); }

TRI_vocbase_t* Syncer::resolveVocbase(VPackSlice const& slice) {
  std::string name;
  if (slice.isObject()) {
    VPackSlice tmp;
    if ((tmp = slice.get(::dbRef)).isString()) {  // wal access protocol
      name = tmp.copyString();
    } else if ((tmp = slice.get(::databaseRef)).isString()) {  // pre 3.3
      name = tmp.copyString();
    }
  } else if (slice.isString()) {
    name = slice.copyString();
  }

  if (name.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                                   "could not resolve vocbase id / name");
  }

  // will work with either names or id's
  auto const& it = _state.vocbases.find(name);

  if (it == _state.vocbases.end()) {
    // automatically checks for id in string
    auto& server = _state.applier._server;
    TRI_vocbase_t* vocbase =
        server.getFeature<DatabaseFeature>().lookupDatabase(name);

    if (vocbase != nullptr) {
      _state.vocbases.try_emplace(name,
                                  *vocbase);  // we can not be lazy because of
                                              // the guard requires a valid ref
    } else {
      LOG_TOPIC("9bb38", DEBUG, Logger::REPLICATION)
          << "could not find database '" << name << "'";
    }

    return vocbase;
  } else {
    return &(it->second.database());
  }
}

std::shared_ptr<LogicalCollection> Syncer::resolveCollection(
    TRI_vocbase_t& vocbase, arangodb::velocypack::Slice const& slice) {
  VPackSlice uuid;

  if ((uuid = slice.get(::cuidRef)).isString()) {
    return vocbase.lookupCollectionByUuid(uuid.copyString());
  } else if ((uuid = slice.get(::globallyUniqueIdRef)).isString()) {
    return vocbase.lookupCollectionByUuid(uuid.copyString());
  }

  // extract "cid"
  DataSourceId cid = ::getCid(slice);

  if (cid.empty()) {
    LOG_TOPIC("fbf1a", ERR, Logger::REPLICATION)
        << "Invalid replication response: Was unable to resolve"
        << " collection from marker: " << slice.toJson();
    return nullptr;
  }

  // extract optional "cname"
  std::string cname = ::getCName(slice);

  if (cname.empty()) {
    cname =
        arangodb::basics::VelocyPackHelper::getStringValue(slice, "name", "");
  }

  return ::getCollectionByIdOrName(vocbase, cid, cname);
}

Result Syncer::applyCollectionDumpMarker(transaction::Methods& trx,
                                         LogicalCollection* coll,
                                         TRI_replication_operation_e type,
                                         VPackSlice const& slice,
                                         std::string& conflictingDocumentKey) {
  if (_state.applier._lockTimeoutRetries > 0) {
    decltype(_state.applier._lockTimeoutRetries) tries = 0;

    while (true) {
      Result res = ::applyCollectionDumpMarkerInternal(
          _state, trx, coll, type, slice, conflictingDocumentKey);

      if (res.errorNumber() != TRI_ERROR_LOCK_TIMEOUT) {
        return res;
      }

      // lock timeout
      if (++tries > _state.applier._lockTimeoutRetries) {
        // timed out
        return res;
      }

      LOG_TOPIC("569c6", DEBUG, Logger::REPLICATION)
          << "got lock timeout while waiting for lock on collection '"
          << coll->name() << "', retrying...";
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      // retry
    }
  } else {
    return ::applyCollectionDumpMarkerInternal(_state, trx, coll, type, slice,
                                               conflictingDocumentKey);
  }
}

/// @brief creates a collection, based on the VelocyPack provided
Result Syncer::createCollection(TRI_vocbase_t& vocbase,
                                arangodb::velocypack::Slice const& slice,
                                LogicalCollection** dst) {
  if (dst != nullptr) {
    *dst = nullptr;
  }

  if (!slice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection slice is no object");
  }

  std::string const name =
      basics::VelocyPackHelper::getStringValue(slice, "name", "");

  if (name.empty()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "no name specified for collection");
  }

  TRI_col_type_e const type = static_cast<TRI_col_type_e>(
      basics::VelocyPackHelper::getNumericValue<int>(slice, "type",
                                                     TRI_COL_TYPE_DOCUMENT));

  // resolve collection by uuid, name, cid (in that order of preference)
  auto col = resolveCollection(vocbase, slice);

  if (col != nullptr && col->type() == type) {
    // collection already exists. TODO: compare attributes
    return Result();
  }

  // conflicting collections need to be dropped from 3.3 onwards
  col = vocbase.lookupCollection(name);

  if (col != nullptr) {
    if (col->system()) {
      SingleCollectionTransaction trx(
          transaction::StandaloneContext::Create(vocbase), *col,
          AccessMode::Type::WRITE);
      trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
      trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
      Result res = trx.begin();

      if (!res.ok()) {
        return res;
      }

      OperationOptions opts;
      OperationResult opRes = trx.truncate(col->name(), opts);

      if (opRes.fail()) {
        return opRes.result;
      }

      return trx.finish(opRes.result);
    } else {
      vocbase.dropCollection(col->id(), false, -1.0);
    }
  }

  VPackSlice uuid = slice.get(StaticStrings::DataSourceGuid);
  // merge in "isSystem" attribute, doesn't matter if name does not start with
  // '_'
  VPackBuilder s;

  s.openObject();
  s.add(StaticStrings::DataSourceSystem, VPackValue(true));

  if (uuid.isString()) {
    // if we received a globallyUniqueId from the remote, then we will always
    // use this id so we can discard the "cid" and "id" values for the
    // collection
    s.add(StaticStrings::DataSourceId, VPackSlice::nullSlice());
    s.add(StaticStrings::DataSourceCid, VPackSlice::nullSlice());
  }

  s.close();

  VPackBuilder merged =
      VPackCollection::merge(slice, s.slice(), /*mergeValues*/ true,
                             /*nullMeansRemove*/ true);

  // we need to remove every occurence of objectId as a key
  auto stripped = rocksutils::stripObjectIds(merged.slice());

  try {
    col = vocbase.createCollection(stripped.first);
  } catch (basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL);
  }

  TRI_ASSERT(col != nullptr);
  TRI_ASSERT(!uuid.isString() || uuid.compareString(col->guid()) == 0);

  if (dst != nullptr) {
    *dst = col.get();
  }

  return Result();
}

/// @brief drops a collection, based on the VelocyPack provided
Result Syncer::dropCollection(VPackSlice const& slice, bool reportError) {
  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto col = resolveCollection(*vocbase, slice);

  if (col == nullptr) {
    if (reportError) {
      return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }

    return Result();
  }

  return vocbase->dropCollection(col->id(), true, -1.0);
}

/// @brief creates an index, based on the VelocyPack provided
Result Syncer::createIndex(VPackSlice const& slice) {
  VPackSlice indexSlice = slice.get("index");
  if (!indexSlice.isObject()) {
    indexSlice = slice.get("data");
  }

  if (!indexSlice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "index slice is not an object");
  }

  TRI_vocbase_t* vocbase = resolveVocbase(slice);

  if (vocbase == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  auto col = resolveCollection(*vocbase, slice);
  if (col == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  "did not find collection for index");
  }

  VPackBuilder s;
  s.openObject();
  s.add(StaticStrings::ObjectId, VPackSlice::nullSlice());
  s.close();
  VPackBuilder merged =
      VPackCollection::merge(indexSlice, s.slice(),
                             /*mergeValues*/ true, /*nullMeansRemove*/ true);

  try {
    VPackSlice idxDef = merged.slice();
    createIndexInternal(idxDef, *col);
  } catch (arangodb::basics::Exception const& ex) {
    return Result(
        ex.code(),
        std::string("caught exception while creating index: ") + ex.what());
  } catch (std::exception const& ex) {
    return Result(
        TRI_ERROR_INTERNAL,
        std::string("caught exception while creating index: ") + ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL,
                  "caught unknown exception while creating index");
  }
  return Result();
}

void Syncer::createIndexInternal(VPackSlice const& idxDef,
                                 LogicalCollection& col) {
  std::shared_ptr<arangodb::Index> idx;
  auto physical = col.getPhysical();
  TRI_ASSERT(physical != nullptr);

  // check any identifier conflicts first
  {
    // check ID first
    IndexId iid = IndexId::none();
    std::string name;  // placeholder for now
    CollectionNameResolver resolver(col.vocbase());
    Result res =
        methods::Indexes::extractHandle(&col, &resolver, idxDef, iid, name);
    if (res.ok() && iid.isSet()) {
      // lookup by id
      auto byId = physical->lookupIndex(iid);
      auto byDef = physical->lookupIndex(idxDef);
      if (byId != nullptr) {
        if (byDef == nullptr || byId != byDef) {
          // drop existing byId
          physical->dropIndex(byId->id());
        } else {
          idx = byId;
        }
      }
    }

    // now check name;
    name = basics::VelocyPackHelper::getStringValue(
        idxDef, StaticStrings::IndexName, "");
    if (!name.empty()) {
      // lookup by name
      auto byName = physical->lookupIndex(name);
      auto byDef = physical->lookupIndex(idxDef);
      if (byName != nullptr) {
        if (byDef == nullptr || byName != byDef) {
          // drop existing byName
          physical->dropIndex(byName->id());
        } else if (idx != nullptr && byName != idx) {
          // drop existing byName and byId
          physical->dropIndex(byName->id());
          physical->dropIndex(idx->id());
          idx = nullptr;
        } else {
          idx = byName;
        }
      }
    }
  }

  if (idx == nullptr) {
    bool created = false;
    idx = physical->createIndex(idxDef, /*restore*/ true, created);
  }
  TRI_ASSERT(idx != nullptr);
}

Result Syncer::dropIndex(arangodb::velocypack::Slice const& slice) {
  auto cb = [&](VPackSlice const& slice) {
    std::string id;

    if (slice.hasKey("data")) {
      id =
          basics::VelocyPackHelper::getStringValue(slice.get("data"), "id", "");
    } else {
      id = basics::VelocyPackHelper::getStringValue(slice, "id", "");
    }

    if (id.empty()) {
      return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                    "id not found in index drop slice");
    }

    IndexId const iid{basics::StringUtils::uint64(id)};
    TRI_vocbase_t* vocbase = resolveVocbase(slice);

    if (vocbase == nullptr) {
      return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    }

    auto col = resolveCollection(*vocbase, slice);

    if (!col) {
      return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }

    try {
      CollectionGuard guard(vocbase, col->id());
      bool result = guard.collection()->dropIndex(iid);

      if (!result) {
        return Result();  // TODO: why do we ignore failures here?
      }

      return Result();
    } catch (arangodb::basics::Exception const& ex) {
      return Result(ex.code(), ex.what());
    } catch (std::exception const& ex) {
      return Result(TRI_ERROR_INTERNAL, ex.what());
    } catch (...) {
      return Result(TRI_ERROR_INTERNAL);
    }
  };

  Result r = cb(slice);

  if (r.fail() && (r.is(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) ||
                   r.is(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND))) {
    // if dropping an index for a non-existing database or collection fails,
    // this is not a real problem
    return Result();
  }

  return r;
}

Result Syncer::changeIndex(VPackSlice const& slice) {
  return {TRI_ERROR_NOT_IMPLEMENTED};
}


/// @brief creates a view, based on the VelocyPack provided
Result Syncer::createView(TRI_vocbase_t& vocbase,
                          arangodb::velocypack::Slice const& slice) {
  if (!slice.isObject()) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "collection slice is no object");
  }

  VPackSlice nameSlice = slice.get(StaticStrings::DataSourceName);

  if (!nameSlice.isString() || nameSlice.getStringLength() == 0) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "no name specified for view");
  }

  VPackSlice guidSlice = slice.get(StaticStrings::DataSourceGuid);

  if (!guidSlice.isString() || guidSlice.getStringLength() == 0) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "no guid specified for view");
  }

  VPackSlice typeSlice = slice.get(StaticStrings::DataSourceType);

  if (!typeSlice.isString() || typeSlice.getStringLength() == 0) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "no type specified for view");
  }

  auto view = vocbase.lookupView(guidSlice.copyString());

  if (view) {  // identical view already exists
    if (!nameSlice.isEqualString(view->name())) {
      auto res = view->rename(nameSlice.copyString());

      if (!res.ok()) {
        return res;
      }
    }

    // always a full-update
    return view->properties(slice, false, false);
  }

  // check for name conflicts
  view = vocbase.lookupView(nameSlice.copyString());
  if (view) {  // resolve name conflict by deleting existing
    Result res = view->drop();
    if (res.fail()) {
      return res;
    }
  }

  VPackBuilder s;

  s.openObject();
  s.add("id", VPackSlice::nullSlice());
  s.close();

  VPackBuilder merged =
      VPackCollection::merge(slice, s.slice(), /*mergeValues*/ true,
                             /*nullMeansRemove*/ true);

  try {
    LogicalView::ptr empty;  // ignore result
    return LogicalView::create(empty, vocbase, merged.slice(), false);
  } catch (basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL);
  }
}

/// @brief drops a view, based on the VelocyPack provided
Result Syncer::dropView(arangodb::velocypack::Slice const& slice,
                        bool /*reportError*/) {
  TRI_vocbase_t* vocbase = resolveVocbase(slice);
  if (vocbase == nullptr) {
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  VPackSlice guidSlice = slice.get(::globallyUniqueIdRef);
  if (guidSlice.isNone()) {
    guidSlice = slice.get(::cuidRef);
  }
  if (!guidSlice.isString() || guidSlice.getStringLength() == 0) {
    return Result(TRI_ERROR_REPLICATION_INVALID_RESPONSE,
                  "no guid specified for view");
  }

  try {
    TRI_ASSERT(!ServerState::instance()->isCoordinator());
    auto view = vocbase->lookupView(guidSlice.copyString());
    if (view) {  // prevent dropping of system views ?
      return view->drop();
    }
  } catch (basics::Exception const& ex) {
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    return Result(TRI_ERROR_INTERNAL);
  }

  return Result();
}

void Syncer::reloadUsers() {
  AuthenticationFeature* af = AuthenticationFeature::instance();
  auth::UserManager* um = af->userManager();
  if (um != nullptr) {
    um->triggerLocalReload();
  }
}

SyncerId Syncer::syncerId() const noexcept { return _state.syncerId; }

}  // namespace arangodb
