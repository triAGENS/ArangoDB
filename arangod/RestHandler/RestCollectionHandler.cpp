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

#include "RestCollectionHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ActionDescription.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/MaintenanceFeature.h"
#include "Cluster/MaintenanceStrings.h"
#include "Cluster/ServerDefaults.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "Logger/LogMacros.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Properties/PlanCollection.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestCollectionHandler::RestCollectionHandler(ArangodServer& server,
                                             GeneralRequest* request,
                                             GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestStatus RestCollectionHandler::execute() {
  switch (_request->requestType()) {
    case rest::RequestType::GET:
      return handleCommandGet();
    case rest::RequestType::POST:
      handleCommandPost();
      break;
    case rest::RequestType::PUT:
      return handleCommandPut();
    case rest::RequestType::DELETE_REQ:
      handleCommandDelete();
      break;
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  }

  return RestStatus::DONE;
}

void RestCollectionHandler::shutdownExecute(bool isFinalized) noexcept {
  if (isFinalized) {
    // reset the transactions so they release all locks as early as possible
    _activeTrx.reset();
    _ctxt.reset();
  }
  RestVocbaseBaseHandler::shutdownExecute(isFinalized);
}

RestStatus RestCollectionHandler::handleCommandGet() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  // /_api/collection
  if (suffixes.empty()) {
    bool excludeSystem = _request->parsedValue("excludeSystem", false);

    _builder.openArray();
    methods::Collections::enumerate(
        &_vocbase, [&](std::shared_ptr<LogicalCollection> const& coll) -> void {
          TRI_ASSERT(coll);
          bool canUse = ExecContext::current().canUseCollection(
              coll->name(), auth::Level::RO);

          if (canUse && (!excludeSystem || !coll->system())) {
            // We do not need a transaction here
            methods::Collections::Context ctxt(coll);

            collectionRepresentation(ctxt,
                                     /*showProperties*/ false,
                                     /*showFigures*/ FiguresType::None,
                                     /*showCount*/ CountType::None);
          }
        });

    _builder.close();
    generateOk(rest::ResponseCode::OK, _builder.slice());

    return RestStatus::DONE;
  }

  std::string const& name = suffixes[0];
  // /_api/collection/<name>
  if (suffixes.size() == 1) {
    try {
      collectionRepresentation(name, /*showProperties*/ false,
                               /*showFigures*/ FiguresType::None,
                               /*showCount*/ CountType::None);
      generateOk(rest::ResponseCode::OK, _builder);
    } catch (basics::Exception const& ex) {  // do not log not found exceptions
      generateError(GeneralResponse::responseCode(ex.code()), ex.code(),
                    ex.what());
    }
    return RestStatus::DONE;
  }

  if (suffixes.size() > 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect GET /_api/collection/<collection-name>/<method>");
    return RestStatus::DONE;
  }

  _builder.clear();

  std::shared_ptr<LogicalCollection> coll;
  auto res = methods::Collections::lookup(_vocbase, name, coll);
  if (res.fail()) {
    generateError(res);
    return RestStatus::DONE;
  }

  TRI_ASSERT(coll);

  std::string const& sub = suffixes[1];

  if (sub == "checksum") {
    // /_api/collection/<identifier>/checksum
    bool withRevisions = _request->parsedValue("withRevisions", false);
    bool withData = _request->parsedValue("withData", false);

    uint64_t checksum;
    RevisionId revId;
    res = methods::Collections::checksum(*coll, withRevisions, withData,
                                         checksum, revId);

    if (res.ok()) {
      {
        VPackObjectBuilder obj(&_builder, true);
        obj->add("checksum", VPackValue(std::to_string(checksum)));
        obj->add("revision", VPackValue(revId.toString()));

        // We do not need a transaction here
        methods::Collections::Context ctxt(coll);

        collectionRepresentation(coll,
                                 /*showProperties*/ false,
                                 /*showFigures*/ FiguresType::None,
                                 /*showCount*/ CountType::None);
      }
      return standardResponse();
    }

    generateError(res);
    return RestStatus::DONE;

  } else if (sub == "figures") {
    // /_api/collection/<identifier>/figures
    bool details = _request->parsedValue("details", false);
    _ctxt = std::make_unique<methods::Collections::Context>(coll);
    return waitForFuture(
        collectionRepresentationAsync(
            *_ctxt,
            /*showProperties*/ true,
            details ? FiguresType::Detailed : FiguresType::Standard,
            CountType::Standard)
            .thenValue([this](futures::Unit&&) { standardResponse(); }));
  } else if (sub == "count") {
    // /_api/collection/<identifier>/count
    initializeTransaction(*coll);
    _ctxt =
        std::make_unique<methods::Collections::Context>(coll, _activeTrx.get());

    bool details = _request->parsedValue("details", false);
    bool checkSyncStatus = _request->parsedValue("checkSyncStatus", false);
    // the checkSyncStatus flag is only set in internal requests performed
    // by the ShardDistributionReporter. it is used to determine the shard
    // synchronization status by asking the maintainance. the functionality
    // is only available on DB servers. as this is an internal API, it is ok
    // to make some assumptions about the requests and let everything else
    // fail.
    if (checkSyncStatus) {
      if (!ServerState::instance()->isDBServer()) {
        generateError(Result(TRI_ERROR_NOT_IMPLEMENTED,
                             "syncStatus API is only available on DB servers"));
        return RestStatus::DONE;
      }

      // details automatically turned off here, as we will not need them
      details = false;

      // check if a SynchronizeShard job is currently executing for the
      // specified shard
      bool isSyncing = server().getFeature<MaintenanceFeature>().hasAction(
          maintenance::EXECUTING, name,
          arangodb::maintenance::SYNCHRONIZE_SHARD);

      // already put some data into the response
      _builder.openObject();
      _builder.add("syncing", VPackValue(isSyncing));
    }

    return waitForFuture(
        collectionRepresentationAsync(
            *_ctxt,
            /*showProperties*/ true,
            /*showFigures*/ FiguresType::None,
            /*showCount*/ details ? CountType::Detailed : CountType::Standard)
            .thenValue([this, checkSyncStatus](futures::Unit&&) {
              if (checkSyncStatus) {
                // checkSyncStatus == true, so we opened the _builder on our own
                // before, and we are now responsible for closing it again.
                _builder.close();
              }
              standardResponse();
            }));
  } else if (sub == "properties") {
    // /_api/collection/<identifier>/properties
    collectionRepresentation(coll,
                             /*showProperties*/ true,
                             /*showFigures*/ FiguresType::None,
                             /*showCount*/ CountType::None);
    return standardResponse();

  } else if (sub == "revision") {
    // /_api/collection/<identifier>/revision

    _ctxt = std::make_unique<methods::Collections::Context>(coll);
    OperationOptions options(_context);
    return waitForFuture(
        methods::Collections::revisionId(*_ctxt, options)
            .thenValue([this, coll](OperationResult&& res) {
              if (res.fail()) {
                generateTransactionError(coll->name(), res);
                return;
              }

              RevisionId rid = RevisionId::fromSlice(res.slice());
              {
                VPackObjectBuilder obj(&_builder, true);
                obj->add("revision", VPackValue(rid.toString()));

                // no need to use async variant
                collectionRepresentation(*_ctxt, /*showProperties*/ true,
                                         FiguresType::None, CountType::None);
              }

              standardResponse();
            }));
  } else if (sub == "shards") {
    // /_api/collection/<identifier>/shards
    if (!ServerState::instance()->isRunningInCluster()) {
      this->generateError(Result(TRI_ERROR_NOT_IMPLEMENTED,
                                 "shards API is only available in a cluster"));
      return RestStatus::DONE;
    }

    {
      VPackObjectBuilder obj(&_builder, true);  // need to open object

      collectionRepresentation(coll,
                               /*showProperties*/ true, FiguresType::None,
                               CountType::None);

      auto& ci = server().getFeature<ClusterFeature>().clusterInfo();
      auto shards = ci.getShardList(std::to_string(coll->planId().id()));

      if (_request->parsedValue("details", false)) {
        // with details
        VPackObjectBuilder arr(&_builder, "shards", true);
        for (ShardID const& shard : *shards) {
          std::vector<ServerID> servers;
          ci.getShardServers(shard, servers);

          if (servers.empty()) {
            continue;
          }

          VPackArrayBuilder arr2(&_builder, shard);

          for (auto const& server : servers) {
            arr2->add(VPackValue(server));
          }
        }
      } else {
        // no details
        VPackArrayBuilder arr(&_builder, "shards", true);

        for (ShardID const& shard : *shards) {
          arr->add(VPackValue(shard));
        }
      }
    }
    return standardResponse();
  }

  generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                "expecting one of the resources 'checksum', 'count', "
                "'figures', 'properties', 'responsibleShard', 'revision', "
                "'shards'");
  return RestStatus::DONE;
}

// create a collection
void RestCollectionHandler::handleCommandPost() {
  if (ServerState::instance()->isDBServer()) {
    generateError(Result(TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR));
    return;
  }

  bool parseSuccess = false;
  VPackSlice const body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    // error message generated in parseVPackBody
    events::CreateCollection(_vocbase.name(), StaticStrings::Empty,
                             TRI_ERROR_BAD_PARAMETER);
    return;
  }
  auto& cluster = _vocbase.server().getFeature<ClusterFeature>();
  bool waitForSyncReplication = _request->parsedValue(
      "waitForSyncReplication", cluster.createWaitsForSyncReplication());

  bool enforceReplicationFactor =
      _request->parsedValue("enforceReplicationFactor", true);

  auto planCollection = PlanCollection::fromCreateAPIBody(
      body, PlanCollection::DatabaseConfiguration{_vocbase});

  if (planCollection.fail()) {
    // error message generated in inspect
    generateError(rest::ResponseCode::BAD, planCollection.errorNumber(),
                  planCollection.errorMessage());
    // Try to get a name for Auditlog, if it is available, otherwise report
    // empty string
    auto collectionName = VelocyPackHelper::getStringView(
        body, StaticStrings::DataSourceName, "");
    events::CreateCollection(_vocbase.name(), collectionName,
                             planCollection.errorNumber());
    return;
  }
  std::vector<PlanCollection> collections{std::move(planCollection.get())};

  OperationOptions options(_context);
  auto result = methods::Collections::create(
      _vocbase,  // collection vocbase
      options, collections,
      waitForSyncReplication,    // replication wait flag
      enforceReplicationFactor,  // replication factor flag
      /*isNewDatabase*/ false    // here always false
  );

  std::shared_ptr<LogicalCollection> coll;
  // backwards compatibility transformation:
  Result res{TRI_ERROR_NO_ERROR};
  if (result.fail()) {
    res = result.result();
  } else {
    TRI_ASSERT(result.get().size() == 1);
    coll = result.get().at(0);
  }

  if (res.ok()) {
    TRI_ASSERT(coll);
    collectionRepresentation(coll->name(),
                             /*showProperties*/ true, FiguresType::None,
                             CountType::None);

    generateOk(rest::ResponseCode::OK, _builder);
  } else {
    generateError(res);
  }
}

RestStatus RestCollectionHandler::handleCommandPut() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();
  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected PUT /_api/collection/<collection-name>/<action>");
    return RestStatus::DONE;
  }
  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    // error message generated in parseVPackBody
    return RestStatus::DONE;
  }

  std::string const& name = suffixes[0];
  std::string const& sub = suffixes[1];

  if (sub != "responsibleShard" && !body.isObject()) {
    // if the caller has sent an empty body. for convenience, let's turn this
    // into an object however, for the "responsibleShard" case we want to
    // distinguish between string values, object values etc. - so we don't do
    // the conversion here
    body = VPackSlice::emptyObjectSlice();
  }

  _builder.clear();

  std::shared_ptr<LogicalCollection> coll;
  Result res = methods::Collections::lookup(_vocbase, name, coll);

  if (res.fail()) {
    generateError(res);
    return RestStatus::DONE;
  }
  TRI_ASSERT(coll);

  if (sub == "load") {
    // "load" is a no-op starting with 3.9
    bool cc = VelocyPackHelper::getBooleanValue(body, "count", true);
    collectionRepresentation(
        name, /*showProperties*/ false,
        /*showFigures*/ FiguresType::None,
        /*showCount*/ cc ? CountType::Standard : CountType::None);
    return standardResponse();
  } else if (sub == "unload") {
    bool flush = _request->parsedValue("flush", false);

    if (flush &&
        TRI_vocbase_col_status_e::TRI_VOC_COL_STATUS_LOADED == coll->status()) {
      server().getFeature<EngineSelectorFeature>().engine().flushWal(false,
                                                                     false);
    }

    // apart from WAL flushing, unload is a no-op starting with 3.9
    collectionRepresentation(name, /*showProperties*/ false,
                             /*showFigures*/ FiguresType::None,
                             /*showCount*/ CountType::None);
    return standardResponse();
  } else if (sub == "compact") {
    coll->compact();

    collectionRepresentation(name, /*showProperties*/ false,
                             /*showFigures*/ FiguresType::None,
                             /*showCount*/ CountType::None);
    return standardResponse();
  } else if (sub == "responsibleShard") {
    if (!ServerState::instance()->isCoordinator()) {
      res.reset(TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR);
      generateError(res);
      return RestStatus::DONE;
    }

    VPackBuilder temp;
    if (body.isString()) {
      temp.openObject();
      temp.add(StaticStrings::KeyString, body);
      temp.close();
      body = temp.slice();
    } else if (body.isNumber()) {
      temp.openObject();
      temp.add(StaticStrings::KeyString,
               VPackValue(std::to_string(body.getNumber<int64_t>())));
      temp.close();
      body = temp.slice();
    }
    if (!body.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "expecting object for responsibleShard");
    }

    std::string shardId;
    res = coll->getResponsibleShard(body, false, shardId);

    if (res.ok()) {
      _builder.openObject();
      _builder.add("shardId", VPackValue(shardId));
      _builder.close();
      return standardResponse();
    } else {
      generateError(res);
      return RestStatus::DONE;
    }

  } else if (sub == "truncate") {
    OperationOptions opts(_context);

    opts.waitForSync =
        _request->parsedValue(StaticStrings::WaitForSyncString, false);
    opts.isSynchronousReplicationFrom =
        _request->value(StaticStrings::IsSynchronousReplicationString);
    opts.truncateCompact = _request->parsedValue(StaticStrings::Compact, true);

    _activeTrx =
        createTransaction(coll->name(), AccessMode::Type::EXCLUSIVE, opts);
    _activeTrx->addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
    _activeTrx->addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
    res = _activeTrx->begin();
    if (res.fail()) {
      generateError(res);
      _activeTrx.reset();
      return RestStatus::DONE;
    }

    if (ServerState::instance()->isDBServer() &&
        (_activeTrx->state()->collection(
             coll->name(), AccessMode::Type::EXCLUSIVE) == nullptr ||
         _activeTrx->state()->isReadOnlyTransaction())) {
      // make sure that the current transaction includes the collection that we
      // want to write into. this is not necessarily the case for follower
      // transactions that are started lazily. in this case, we must reject the
      // request. we _cannot_ do this for follower transactions, where shards
      // may lazily be added (e.g. if servers A and B both replicate their own
      // write ops to follower C one after the after, then C will first see only
      // shards from A and then only from B).
      res.reset(TRI_ERROR_TRANSACTION_UNREGISTERED_COLLECTION,
                std::string("Transaction with id '") +
                    std::to_string(_activeTrx->tid().id()) +
                    "' does not contain collection '" + coll->name() +
                    "' with the required access mode.");
      generateError(res);
      _activeTrx.reset();
      return RestStatus::DONE;
    }

    return waitForFuture(
        _activeTrx->truncateAsync(coll->name(), opts)
            .thenValue([this, coll, opts](OperationResult&& opres) {
              // Will commit if no error occured.
              // or abort if an error occured.
              // result stays valid!
              Result res = _activeTrx->finish(opres.result);
              if (opres.fail()) {
                generateTransactionError(coll->name(), opres);
                return;
              }

              if (res.fail()) {
                generateTransactionError(
                    coll->name(), OperationResult(res, opres.options), "");
                return;
              }

              _activeTrx.reset();

              if (opts.truncateCompact) {
                // wait for the transaction to finish first. only after that
                // compact the data range(s) for the collection we shouldn't run
                // compact() as part of the transaction, because the compact
                // will be useless inside due to the snapshot the transaction
                // has taken
                coll->compact();
              }

              // no need to use async method, no
              collectionRepresentation(coll,
                                       /*showProperties*/ false,
                                       /*showFigures*/ FiguresType::None,
                                       /*showCount*/ CountType::None);
              standardResponse();
            }));

  } else if (sub == "properties") {
    std::vector<std::string> keep = {
        StaticStrings::WaitForSyncString,    StaticStrings::Schema,
        StaticStrings::ReplicationFactor,
        StaticStrings::MinReplicationFactor,  // deprecated
        StaticStrings::WriteConcern,         StaticStrings::ComputedValues,
        StaticStrings::CacheEnabled};
    VPackBuilder props = VPackCollection::keep(body, keep);

    OperationOptions options(_context);
    res = methods::Collections::updateProperties(*coll, props.slice(), options);
    if (res.fail()) {
      generateError(res);
      return RestStatus::DONE;
    }

    collectionRepresentation(name, /*showProperties*/ true,
                             /*showFigures*/ FiguresType::None,
                             /*showCount*/ CountType::None);
    return standardResponse();

  } else if (sub == "rename") {
    VPackSlice const newNameSlice = body.get(StaticStrings::DataSourceName);
    if (!newNameSlice.isString()) {
      generateError(Result(TRI_ERROR_ARANGO_ILLEGAL_NAME, "name is empty"));
      return RestStatus::DONE;
    }

    std::string const newName = newNameSlice.copyString();
    res = methods::Collections::rename(*coll, newName, false);

    if (res.ok()) {
      collectionRepresentation(newName, /*showProperties*/ false,
                               /*showFigures*/ FiguresType::None,
                               /*showCount*/ CountType::None);
      return standardResponse();
    }

    generateError(res);
    return RestStatus::DONE;

  } else if (sub == "loadIndexesIntoMemory") {
    OperationOptions options(_context);
    return waitForFuture(methods::Collections::warmup(_vocbase, *coll)
                             .thenValue([this, coll, options](Result&& res) {
                               if (res.fail()) {
                                 generateTransactionError(
                                     coll->name(),
                                     OperationResult(res, options), "");
                                 return;
                               }

                               {
                                 VPackObjectBuilder obj(&_builder, true);
                                 obj->add("result", VPackValue(res.ok()));
                               }

                               standardResponse();
                             }));
  }

  res = handleExtraCommandPut(coll, sub, _builder);
  if (res.is(TRI_ERROR_NOT_IMPLEMENTED)) {
    res.reset(TRI_ERROR_HTTP_NOT_FOUND,
              "expecting one of the actions 'load', 'unload', 'truncate',"
              " 'properties', 'compact', 'rename', 'loadIndexesIntoMemory'");
    generateError(res);
  } else if (res.fail()) {
    generateError(res);
  } else {
    standardResponse();
  }

  return RestStatus::DONE;
}

void RestCollectionHandler::handleCommandDelete() {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected DELETE /_api/collection/<collection-name>");
    events::DropCollection(_vocbase.name(), "", TRI_ERROR_HTTP_BAD_PARAMETER);
    return;
  }

  std::string const& name = suffixes[0];
  bool allowDropSystem =
      _request->parsedValue(StaticStrings::DataSourceSystem, false);
  _builder.clear();

  std::shared_ptr<LogicalCollection> coll;
  Result res = methods::Collections::lookup(_vocbase, name, coll);
  if (res.fail()) {
    events::DropCollection(_vocbase.name(), name, res.errorNumber());
    generateError(res);
    return;
  }
  TRI_ASSERT(coll);

  {
    VPackObjectBuilder obj(&_builder, true);

    obj->add("id", VPackValue(std::to_string(coll->id().id())));
    res = methods::Collections::drop(*coll, allowDropSystem, -1.0);
  }

  if (res.fail()) {
    generateError(res);
  } else {
    generateOk(rest::ResponseCode::OK, _builder);
  }
}

/// generate collection info. We lookup the collection again, because in the
/// cluster someinfo is lazily added in loadPlan, which means load, unload,
/// truncate
/// and create will not immediately show the expected results on a collection
/// object.
void RestCollectionHandler::collectionRepresentation(std::string const& name,
                                                     bool showProperties,
                                                     FiguresType showFigures,
                                                     CountType showCount) {
  std::shared_ptr<LogicalCollection> coll;
  Result r = methods::Collections::lookup(_vocbase, name, coll);
  if (r.fail()) {
    THROW_ARANGO_EXCEPTION(r);
  }
  TRI_ASSERT(coll);
  collectionRepresentation(coll, showProperties, showFigures, showCount);
}

void RestCollectionHandler::collectionRepresentation(
    std::shared_ptr<LogicalCollection> coll, bool showProperties,
    FiguresType showFigures, CountType showCount) {
  if (showProperties || showCount != CountType::None) {
    // Here we need a transaction
    initializeTransaction(*coll);
    methods::Collections::Context ctxt(coll, _activeTrx.get());

    collectionRepresentation(ctxt, showProperties, showFigures, showCount);
  } else {
    // We do not need a transaction here
    methods::Collections::Context ctxt(coll);

    collectionRepresentation(ctxt, showProperties, showFigures, showCount);
  }
}

void RestCollectionHandler::collectionRepresentation(
    methods::Collections::Context& ctxt, bool showProperties,
    FiguresType showFigures, CountType showCount) {
  collectionRepresentationAsync(ctxt, showProperties, showFigures, showCount)
      .get();
}

futures::Future<futures::Unit>
RestCollectionHandler::collectionRepresentationAsync(
    methods::Collections::Context& ctxt, bool showProperties,
    FiguresType showFigures, CountType showCount) {
  bool wasOpen = _builder.isOpenObject();
  if (!wasOpen) {
    _builder.openObject();
  }

  auto coll = ctxt.coll();
  TRI_ASSERT(coll != nullptr);

  // `methods::Collections::properties` will filter these out
  _builder.add(StaticStrings::DataSourceId,
               VPackValue(std::to_string(coll->id().id())));
  _builder.add(StaticStrings::DataSourceName, VPackValue(coll->name()));
  _builder.add("status", VPackValue(coll->status()));
  _builder.add(StaticStrings::DataSourceType, VPackValue(coll->type()));

  if (!showProperties) {
    _builder.add(StaticStrings::DataSourceSystem, VPackValue(coll->system()));
    _builder.add(StaticStrings::DataSourceGuid, VPackValue(coll->guid()));
  } else {
    Result res = methods::Collections::properties(ctxt, _builder);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }

  OperationOptions options(_context);
  futures::Future<OperationResult> figures =
      futures::makeFuture(OperationResult(Result(), options));
  if (showFigures != FiguresType::None) {
    figures = coll->figures(showFigures == FiguresType::Detailed, options);
  }

  return std::move(figures)
      .thenValue([=, this, &ctxt](OperationResult&& figures)
                     -> futures::Future<OperationResult> {
        if (figures.fail()) {
          THROW_ARANGO_EXCEPTION(figures.result);
        }
        if (figures.buffer) {
          _builder.add("figures", figures.slice());
        }

        if (showCount != CountType::None) {
          auto trx = ctxt.trx(AccessMode::Type::READ, true, true);
          TRI_ASSERT(trx != nullptr);
          return trx->countAsync(coll->name(),
                                 showCount == CountType::Detailed
                                     ? transaction::CountType::Detailed
                                     : transaction::CountType::Normal,
                                 options);
        }
        return futures::makeFuture(OperationResult(Result(), options));
      })
      .thenValue([=, this, &ctxt](OperationResult&& opRes) -> void {
        if (opRes.fail()) {
          if (showCount != CountType::None) {
            auto trx = ctxt.trx(AccessMode::Type::READ, true, true);
            TRI_ASSERT(trx != nullptr);
            std::ignore = trx->finish(opRes.result);
          }
          THROW_ARANGO_EXCEPTION(opRes.result);
        }

        if (showCount != CountType::None) {
          _builder.add("count", opRes.slice());
        }

        if (!wasOpen) {
          _builder.close();
        }
      });
}

RestStatus RestCollectionHandler::standardResponse() {
  generateOk(rest::ResponseCode::OK, _builder);
  _response->setHeaderNC(StaticStrings::Location,
                         "/_db/" + StringUtils::urlEncode(_vocbase.name()) +
                             _request->requestPath());
  return RestStatus::DONE;
}

void RestCollectionHandler::initializeTransaction(LogicalCollection& coll) {
  try {
    _activeTrx = createTransaction(coll.name(), AccessMode::Type::READ,
                                   OperationOptions());
  } catch (basics::Exception const& ex) {
    if (ex.code() == TRI_ERROR_TRANSACTION_NOT_FOUND) {
      // this will happen if the tid of a managed transaction is passed in,
      // but the transaction hasn't yet started on the DB server. in
      // this case, we create an ad-hoc transaction on the underlying
      // collection
      _activeTrx = std::make_unique<SingleCollectionTransaction>(
          transaction::StandaloneContext::Create(_vocbase), coll.name(),
          AccessMode::Type::READ);
    } else {
      throw;
    }
  }

  TRI_ASSERT(_activeTrx != nullptr);
  Result res = _activeTrx->begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }
}
