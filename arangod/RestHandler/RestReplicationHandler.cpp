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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "RestReplicationHandler.h"

#include "Agency/AgencyComm.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryMethods.h"
#include "Aql/QueryResult.h"
#include "Aql/QueryString.h"
#include "Auth/UserManager.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/RocksDBUtils.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/hashes.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterHelpers.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ClusterIndexMethods.h"
#include "Cluster/CollectionInfoCurrent.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/RebootTracker.h"
#include "Cluster/ResignShardLeadership.h"
#include "Cluster/ServerState.h"
#include "Containers/HashSet.h"
#include "Containers/MerkleTree.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "Indexes/Index.h"
#include "Metrics/Counter.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Replication/DatabaseInitialSyncer.h"
#include "Replication/DatabaseReplicationApplier.h"
#include "Replication/GlobalInitialSyncer.h"
#include "Replication/GlobalReplicationApplier.h"
#include "Replication/ReplicationApplierConfiguration.h"
#include "Replication/ReplicationClients.h"
#include "Replication/ReplicationFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "RocksDBEngine/RocksDBCollection.h"
#include "Sharding/ShardingInfo.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Manager.h"
#include "Transaction/ManagerFeature.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utilities/NameValidator.h"
#include "VocBase/Identifiers/RevisionId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/Properties/CreateCollectionBody.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/CollectionCreationInfo.h"
#include "VocBase/Properties/DatabaseConfiguration.h"

#include <absl/strings/str_cat.h>
#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/Slice.h>
#include <velocypack/Validator.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;
using namespace arangodb::cluster;
using Helper = arangodb::basics::VelocyPackHelper;

namespace {
std::string const dataString("data");
std::string const typeString("type");

bool ignoreHiddenEnterpriseCollection(std::string const& name, bool force) {
#ifdef USE_ENTERPRISE
  if (!force && name[0] == '_') {
    if (name.starts_with(StaticStrings::FullLocalPrefix) ||
        name.starts_with(StaticStrings::FullFromPrefix) ||
        name.starts_with(StaticStrings::FullToPrefix)) {
      LOG_TOPIC("944c4", WARN, arangodb::Logger::REPLICATION)
          << "Restore ignoring collection " << name
          << ". Will be created via SmartGraphs of a full dump. If you want to "
          << "restore ONLY this collection use 'arangorestore --force'. "
          << "However this is not recommended and you should instead restore "
          << "the EdgeCollection of the SmartGraph instead.";
      return true;
    }
  }
#endif
  return false;
}

/**
 * Handle existing collections for restore.
 *
 * @param vocbase Database we work in
 * @param name  Name of the collection
 * @param dropExisting Flag if we are allowed to drop or truncate the collection
 * @return An error if the original lookup failed or we attempted do truncate or
 * drop the collection and the operation failed, otherwise it returns true if
 * the collection exists after this call, and false if it does not.
 */
auto handlingOfExistingCollection(TRI_vocbase_t& vocbase,
                                  std::string const& name, bool dropExisting)
    -> futures::Future<ResultT<bool>> {
  ExecContextSuperuserScope escope(
      ExecContext::current().isSuperuser() ||
      (ExecContext::current().isAdminUser() && !ServerState::readOnly()));

  std::shared_ptr<LogicalCollection> col;
  auto lookupResult = methods::Collections::lookup(vocbase, name, col);

  if (lookupResult.ok()) {
    TRI_ASSERT(col);
    if (dropExisting) {
      try {
        if (ServerState::instance()->isCoordinator() &&
            name == StaticStrings::AnalyzersCollection &&
            vocbase.server()
                .hasFeature<iresearch::IResearchAnalyzerFeature>()) {
          // We have ArangoSearch here. So process analyzers accordingly.
          // We can`t just recreate/truncate collection. Agency should be
          // properly notified analyzers are gone.
          // The single server and DBServer case is handled after restore of
          // data.
          auto res = vocbase.server()
                         .getFeature<iresearch::IResearchAnalyzerFeature>()
                         .removeAllAnalyzers(
                             vocbase, transaction::OperationOriginInternal{
                                          "removing analyzers"});
          if (res.ok()) {
            // Analyzers are only truncated, never dropped, so collection still
            // exists.
            co_return {true};
          }
          co_return res;
        }

        CollectionDropOptions dropOptions{.allowDropSystem = true,
                                          .keepUserRights = true,
                                          .allowDropGraphCollection = true};
        auto dropResult = methods::Collections::drop(*col, dropOptions);
        if (dropResult.fail()) {
          if (dropResult.is(TRI_ERROR_FORBIDDEN) ||
              dropResult.is(
                  TRI_ERROR_CLUSTER_MUST_NOT_DROP_COLL_OTHER_DISTRIBUTESHARDSLIKE)) {
            // If we are not allowed to drop the collection.
            // Try to truncate.
            auto origin = transaction::OperationOriginInternal{
                "truncating collection for restore"};
            auto ctx = transaction::StandaloneContext::create(vocbase, origin);
            SingleCollectionTransaction trx(std::move(ctx), *col,
                                            AccessMode::Type::EXCLUSIVE);

            trx.addHint(transaction::Hints::Hint::INTERMEDIATE_COMMITS);
            trx.addHint(transaction::Hints::Hint::ALLOW_RANGE_DELETE);
            auto trxRes = co_await trx.beginAsync();

            if (!trxRes.ok()) {
              co_return trxRes;
            }

            OperationOptions options;
            OperationResult opRes = trx.truncate(name, options);

            auto res = trx.finish(opRes.result);
            if (!res.ok()) {
              co_return res;
            }
            // After a truncate the collection still exists.
            co_return {true};
          }

          co_return Result(dropResult.errorNumber(),
                           arangodb::basics::StringUtils::concatT(
                               "unable to drop collection '", name,
                               "': ", dropResult.errorMessage()));
        }

        // we just removed the collection, so we cannot rely on it being present
        // now
        col.reset();
      } catch (basics::Exception const& ex) {
        LOG_TOPIC("41579", DEBUG, Logger::REPLICATION)
            << "processRestoreCollection "
            << "could not drop collection: " << ex.what();
        // We return false (collection does not exist) here
        // in order to trigger a creation step later, which should produce
        // a duplicate name error. (Unless someone raced and dropped the
        // collection)
        // Returning true would yield a success of this method.
        co_return {false};
      } catch (...) {
        // We return false (collection does not exist) here
        // in order to trigger a creation step later, which should produce
        // a duplicate name error. (Unless someone raced and dropped the
        // collection)
        // Returning true would yield a success of this method.
        LOG_TOPIC("41580", DEBUG, Logger::REPLICATION)
            << "processRestoreCollection "
            << "could not drop collection: unknown error";
        co_return {false};
      }
    } else {
      // Found a collection, and we are not allowed to drop it, so it exists.
      co_return {true};
    }
  } else if (lookupResult.isNot(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND)) {
    co_return lookupResult;
  }
  // If we end up here the collection does not exist.
  co_return {false};
}

}  // namespace

std::chrono::hours const RestReplicationHandler::_tombstoneTimeout =
    std::chrono::hours(24);

basics::ReadWriteLock RestReplicationHandler::_tombLock;
std::unordered_map<std::string,
                   std::chrono::time_point<std::chrono::steady_clock>>
    RestReplicationHandler::_tombstones = {};

static TransactionId ExtractReadlockId(VPackSlice slice) {
  TRI_ASSERT(slice.isString());
  return TransactionId{StringUtils::uint64(slice.copyString())};
}

static Result checkPlanLeaderDirect(
    std::shared_ptr<LogicalCollection> const& col,
    std::string const& claimLeaderId) {
  std::vector<std::string> agencyPath = {"Plan",
                                         "Collections",
                                         col->vocbase().name(),
                                         std::to_string(col->planId().id()),
                                         "shards",
                                         col->name()};

  std::string shardAgencyPathString = StringUtils::join(agencyPath, '/');

  AgencyComm ac(col->vocbase().server());
  AgencyCommResult res = ac.getValues(shardAgencyPathString);

  if (res.successful()) {
    // This is bullshit. Why does the *fancy* AgencyComm Manager
    // prepend the agency url with `arango` but in the end returns an object
    // that is prepended by `arango`! WTF!?
    VPackSlice plan =
        res.slice().at(0).get(AgencyCommHelper::path()).get(agencyPath);

    // The collection might be deleted in the meantime
    if (plan.isArray() && plan.length() > 0) {
      VPackSlice leaderSlice = plan.at(0);
      TRI_ASSERT(leaderSlice.isString());
      if (leaderSlice.isEqualString(claimLeaderId)) {
        return Result{};
      }
    }
  }

  return Result{TRI_ERROR_FORBIDDEN};
}

static Result restoreDataParser(std::string_view currentLine,
                                std::string const& collectionName, int line,
                                VPackBuilder& builder, VPackSlice& doc,
                                TRI_replication_operation_e& type) {
  TRI_ASSERT(!currentLine.empty());
  builder.clear();

  try {
    VPackParser parser(builder, builder.options);
    parser.parse(currentLine.data(), currentLine.size());
  } catch (std::exception const& ex) {
    // Could not even build the string
    return Result{
        TRI_ERROR_HTTP_CORRUPTED_JSON,
        absl::StrCat("received invalid JSON data for collection '",
                     collectionName, "' on line ", line, ": ", ex.what())};
  } catch (...) {
    return Result{TRI_ERROR_INTERNAL};
  }

  VPackSlice slice = builder.slice();

  if (!slice.isObject()) {
    return Result{TRI_ERROR_HTTP_CORRUPTED_JSON,
                  absl::StrCat("received invalid JSON data for collection '",
                               collectionName, "' on line ", line,
                               ": data is no object")};
  }

  type = REPLICATION_INVALID;

  VPackSlice value = slice.get(StaticStrings::KeyString);
  if (value.isString()) {
    // compact format
    type = REPLICATION_MARKER_DOCUMENT;
    // for a valid document marker without envelope, doc points to the actual
    // docuemnt
    doc = slice;
  } else {
    // enveloped (old) format. each document is wrapped into a
    // {"type":2300,"data":{...}} envelope
    value = slice.get(::typeString);
    if (value.isNumber()) {
      int v = value.getNumericValue<int>();
      if (v == 2301) {
        // pre-3.0 type for edges
        type = REPLICATION_MARKER_DOCUMENT;
      } else {
        type = static_cast<TRI_replication_operation_e>(v);
      }

      if (type == REPLICATION_MARKER_DOCUMENT) {
        // for dumps taken with RocksDB and/or MMFiles
        value = slice.get(::dataString);
        if (!value.isObject()) {
          type = REPLICATION_INVALID;
        } else {
          // for a valid document marker, doc points to the actual docuemnt
          doc = value;
        }
      } else if (type == REPLICATION_MARKER_REMOVE) {
        // edge case: only needed for old dumps taken with MMFiles
        value = slice.get(::dataString);
        if (value.isObject()) {
          value = value.get(StaticStrings::KeyString);
        } else {
          value = slice.get("key");
        }
        if (!value.isString()) {
          type = REPLICATION_INVALID;
        } else {
          // for a valid remove marker, doc points to the key string
          doc = value;
        }
      }
    }
  }

  TRI_ASSERT(type != REPLICATION_MARKER_DOCUMENT || doc.isObject());

  if (type != REPLICATION_MARKER_DOCUMENT &&
      type != REPLICATION_MARKER_REMOVE) {
    // we can only handle REPLICATION_MARKER_DOCUMENT and
    // REPLICATION_MARKER_REMOVE here
    return Result{TRI_ERROR_HTTP_CORRUPTED_JSON,
                  absl::StrCat("received invalid JSON data for collection '",
                               collectionName, "' on line ", line,
                               ": got an invalid input document")};
  }

  return {};
}

RestReplicationHandler::RestReplicationHandler(ArangodServer& server,
                                               GeneralRequest* request,
                                               GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an error if called on a coordinator server
////////////////////////////////////////////////////////////////////////////////

bool RestReplicationHandler::isCoordinatorError() {
  if (ServerState::instance()->isCoordinator()) {
    generateError(rest::ResponseCode::NOT_IMPLEMENTED,
                  TRI_ERROR_CLUSTER_UNSUPPORTED,
                  "replication API is not supported on a coordinator");

    return true;
  }

  return false;
}

std::string const RestReplicationHandler::LoggerState = "logger-state";
std::string const RestReplicationHandler::LoggerTickRanges =
    "logger-tick-ranges";
std::string const RestReplicationHandler::LoggerFirstTick = "logger-first-tick";
std::string const RestReplicationHandler::LoggerFollow = "logger-follow";
std::string const RestReplicationHandler::Batch = "batch";
std::string const RestReplicationHandler::Inventory = "inventory";
std::string const RestReplicationHandler::Keys = "keys";
std::string const RestReplicationHandler::Revisions = "revisions";
std::string const RestReplicationHandler::Tree = "tree";
std::string const RestReplicationHandler::Ranges = "ranges";
std::string const RestReplicationHandler::Documents = "documents";
std::string const RestReplicationHandler::Dump = "dump";
std::string const RestReplicationHandler::RestoreCollection =
    "restore-collection";
std::string const RestReplicationHandler::RestoreIndexes = "restore-indexes";
std::string const RestReplicationHandler::RestoreData = "restore-data";
std::string const RestReplicationHandler::RestoreView = "restore-view";
std::string const RestReplicationHandler::Sync = "sync";
std::string const RestReplicationHandler::MakeFollower = "make-follower";
std::string const RestReplicationHandler::ServerId = "server-id";
std::string const RestReplicationHandler::ApplierConfig = "applier-config";
std::string const RestReplicationHandler::ApplierStart = "applier-start";
std::string const RestReplicationHandler::ApplierStop = "applier-stop";
std::string const RestReplicationHandler::ApplierState = "applier-state";
std::string const RestReplicationHandler::ApplierStateAll = "applier-state-all";
std::string const RestReplicationHandler::ClusterInventory = "clusterInventory";
std::string const RestReplicationHandler::AddFollower = "addFollower";
std::string const RestReplicationHandler::RemoveFollower = "removeFollower";
std::string const RestReplicationHandler::SetTheLeader = "set-the-leader";
std::string const RestReplicationHandler::HoldReadLockCollection =
    "holdReadLockCollection";

// main function that dispatches the different routes and commands
RestStatus RestReplicationHandler::execute() {
  auto res = testPermissions();
  if (!res.ok()) {
    generateError(res);
    return RestStatus::DONE;
  }
  // extract the request type
  auto const type = _request->requestType();
  auto const& suffixes = _request->suffixes();

  size_t const len = suffixes.size();

  if (len >= 1) {
    std::string const& command = suffixes[0];

    if (command == LoggerState) {
      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      handleCommandLoggerState();
    } else if (command == LoggerTickRanges) {
      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }
      handleCommandLoggerTickRanges();
    } else if (command == LoggerFirstTick) {
      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }
      handleCommandLoggerFirstTick();
    } else if (command == LoggerFollow) {
      if (type != rest::RequestType::GET && type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }
      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }
      // track the number of parallel invocations of the tailing API
      auto& rf = _vocbase.server().getFeature<ReplicationFeature>();
      // this may throw when too many threads are going into tailing
      rf.trackTailingStart();

      auto guard = scopeGuard([&rf]() noexcept { rf.trackTailingEnd(); });

      handleCommandLoggerFollow();
    } else if (command == Batch) {
      // access batch context in context manager
      // example call: curl -XPOST --dump - --data '{}'
      // http://localhost:5555/_api/replication/batch
      // the object may contain a "ttl" for the context

      // POST - create batch id / handle
      // PUT  - extend batch id / handle
      // DEL  - delete batchid

      if (ServerState::instance()->isCoordinator()) {
        handleUnforwardedTrampolineCoordinator();
      } else {
        return waitForFuture(handleCommandBatch());
      }
    } else if (command == Inventory) {
      // get overview of collections and indexes followed by some extra data
      // example call: curl --dump -
      // http://localhost:5555/_api/replication/inventory?batchId=75

      // {
      //    collections : [ ... ],
      //    "state" : {
      //      "running" : true,
      //      "lastLogTick" : "10528",
      //      "lastUncommittedLogTick" : "10531",
      //      "totalEvents" : 3782,
      //      "time" : "2017-07-19T21:50:59Z"
      //    },
      //   "tick" : "10531"
      // }

      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      if (ServerState::instance()->isCoordinator()) {
        handleUnforwardedTrampolineCoordinator();
      } else {
        handleCommandInventory();
      }
    } else if (command == Keys) {
      // preconditions for calling this route are unclear and undocumented --
      // FIXME
      if (type != rest::RequestType::GET && type != rest::RequestType::POST &&
          type != rest::RequestType::PUT &&
          type != rest::RequestType::DELETE_REQ) {
        goto BAD_CALL;
      }

      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }

      if (type == rest::RequestType::POST) {
        // has to be called first will bind the iterator to a collection

        // example: curl -XPOST --dump -
        // 'http://localhost:5555/_db/_system/_api/replication/keys/?collection=_users&batchId=169'
        // ; echo
        // returns
        // { "id": <context id - int>,
        //   "count": <number of documents in collection - int>
        // }
        return waitForFuture(handleCommandCreateKeys());
      } else if (type == rest::RequestType::GET) {
        // curl --dump -
        // 'http://localhost:5555/_db/_system/_api/replication/keys/123?collection=_users'
        // ; echo # id is batchid
        handleCommandGetKeys();
      } else if (type == rest::RequestType::PUT) {
        handleCommandFetchKeys();
      } else if (type == rest::RequestType::DELETE_REQ) {
        handleCommandRemoveKeys();
      }
    } else if (command == Revisions) {
      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }

      if (len > 1) {
        std::string const& subCommand = suffixes[1];
        if (type == rest::RequestType::GET && subCommand == Tree) {
          handleCommandRevisionTree();
        } else if (type == rest::RequestType::POST && subCommand == Tree) {
          return waitForFuture(handleCommandRebuildRevisionTree());
#ifdef ARANGODB_ENABLE_FAILURE_TESTS
        } else if (type == rest::RequestType::PUT && subCommand == Tree) {
          handleCommandCorruptRevisionTree();
#endif
        } else if (type == rest::RequestType::PUT && subCommand == Ranges) {
          handleCommandRevisionRanges();
        } else if (type == rest::RequestType::PUT && subCommand == Documents) {
          handleCommandRevisionDocuments();
        }
      } else {
        goto BAD_CALL;
      }
    } else if (command == Dump) {
      // works on collections
      // example: curl --dump -
      // 'http://localhost:5555/_db/_system/_api/replication/dump?collection=test&batchId=115'
      // requires batch-id
      // does internally an
      //   - get inventory
      //   - purge local
      //   - dump remote to local

      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }

      if (ServerState::instance()->isCoordinator()) {
        handleUnforwardedTrampolineCoordinator();
      } else {
        handleCommandDump();
      }
    } else if (command == RestoreCollection) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      return waitForFuture(handleCommandRestoreCollection());
    } else if (command == RestoreIndexes) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      handleCommandRestoreIndexes();
    } else if (command == RestoreData) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }
      return waitForFuture(handleCommandRestoreData());
    } else if (command == RestoreView) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      handleCommandRestoreView();
    } else if (command == Sync) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }

      handleCommandSync();
    } else if (command == MakeFollower ||
               command == "make-slave" /*deprecated*/) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }

      handleCommandMakeFollower();
    } else if (command == ServerId) {
      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      handleCommandServerId();
    } else if (command == ApplierConfig) {
      if (type == rest::RequestType::GET) {
        handleCommandApplierGetConfig();
      } else {
        if (type != rest::RequestType::PUT) {
          goto BAD_CALL;
        }
        handleCommandApplierSetConfig();
      }
    } else if (command == ApplierStart) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }

      handleCommandApplierStart();
    } else if (command == ApplierStop) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }

      if (isCoordinatorError()) {
        return RestStatus::DONE;
      }

      handleCommandApplierStop();
    } else if (command == ApplierState) {
      if (type == rest::RequestType::DELETE_REQ) {
        handleCommandApplierDeleteState();
      } else {
        if (type != rest::RequestType::GET) {
          goto BAD_CALL;
        }
        handleCommandApplierGetState();
      }
    } else if (command == ApplierStateAll) {
      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      handleCommandApplierGetStateAll();
    } else if (command == ClusterInventory) {
      if (type != rest::RequestType::GET) {
        goto BAD_CALL;
      }
      if (!ServerState::instance()->isCoordinator()) {
        generateError(rest::ResponseCode::FORBIDDEN,
                      TRI_ERROR_CLUSTER_ONLY_ON_COORDINATOR);
      } else {
        handleCommandClusterInventory();
      }
    } else if (command == AddFollower) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }
      if (!ServerState::instance()->isDBServer()) {
        generateError(rest::ResponseCode::FORBIDDEN,
                      TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
      } else {
        return waitForFuture(handleCommandAddFollower());
      }
    } else if (command == RemoveFollower) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }
      if (!ServerState::instance()->isDBServer()) {
        generateError(rest::ResponseCode::FORBIDDEN,
                      TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
      } else {
        handleCommandRemoveFollower();
      }
    } else if (command == SetTheLeader) {
      if (type != rest::RequestType::PUT) {
        goto BAD_CALL;
      }
      if (!ServerState::instance()->isDBServer()) {
        generateError(rest::ResponseCode::FORBIDDEN,
                      TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
      } else {
        handleCommandSetTheLeader();
      }
    } else if (command == HoldReadLockCollection) {
      if (!ServerState::instance()->isDBServer()) {
        generateError(rest::ResponseCode::FORBIDDEN,
                      TRI_ERROR_CLUSTER_ONLY_ON_DBSERVER);
      } else {
        if (type == rest::RequestType::POST) {
          return waitForFuture(handleCommandHoldReadLockCollection());
        } else if (type == rest::RequestType::DELETE_REQ) {
          handleCommandCancelHoldReadLockCollection();
        } else if (type == rest::RequestType::GET) {
          handleCommandGetIdForReadLockCollection();
        } else {
          goto BAD_CALL;
        }
      }
    } else {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    std::string("invalid command '") + command + "'");
    }

    return RestStatus::DONE;
  }

BAD_CALL:
  if (len != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_SUPERFLUOUS_SUFFICES,
                  "expecting URL /_api/replication/<command>");
  } else {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  }

  return RestStatus::DONE;
}

Result RestReplicationHandler::testPermissions() {
  if (!_request->authenticated()) {
    return TRI_ERROR_NO_ERROR;
  }

  std::string user = _request->user();
  auto const& suffixes = _request->suffixes();
  size_t const len = suffixes.size();
  if (len >= 1) {
    auto const type = _request->requestType();
    std::string const& command = suffixes[0];
    if ((command == Batch) ||
        (command == Inventory && type == rest::RequestType::GET) ||
        (command == Dump && type == rest::RequestType::GET) ||
        (command == RestoreCollection && type == rest::RequestType::PUT)) {
      if (command == Dump) {
        // check dump collection permissions (at least ro needed)
        std::string collectionName = _request->value("collection");

        if (ServerState::instance()->isCoordinator()) {
          // We have a shard id, need to translate.
          // This API is explicitly called with Shards not with Collections.
          ClusterInfo& ci = server().getFeature<ClusterFeature>().clusterInfo();
          auto maybeShardID = ShardID::shardIdFromString(collectionName);
          if (maybeShardID.fail()) {
            // Compatibility with old API, which would return
            // an empty collection name if we were not handing in a shard.
            return Result{TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND};
          }
          collectionName = ci.getCollectionNameForShard(maybeShardID.get());
        }

        if (!collectionName.empty()) {
          auto& exec = ExecContext::current();
          ExecContextSuperuserScope escope(exec.isAdminUser());
          if (!exec.isAdminUser() &&
              !exec.canUseCollection(collectionName, auth::Level::RO)) {
            // not enough rights
            return Result(TRI_ERROR_FORBIDDEN);
          }
        } else {
          // not found, return 404
          return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
        }
      } else if (command == RestoreCollection) {
        VPackSlice const slice = _request->payload();
        VPackSlice const parameters = slice.get("parameters");
        if (parameters.isObject()) {
          if (parameters.get("name").isString()) {
            std::string collectionName = parameters.get("name").copyString();
            if (!collectionName.empty()) {
              std::string dbName = _request->databaseName();
              DatabaseFeature& databaseFeature =
                  _vocbase.server().getFeature<DatabaseFeature>();
              auto vocbase = databaseFeature.useDatabase(dbName);
              if (vocbase == nullptr) {
                return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
              }

              std::string const& overwriteCollection =
                  _request->value("overwrite");

              auto& exec = ExecContext::current();
              ExecContextSuperuserScope escope(exec.isAdminUser());

              if (overwriteCollection == "true" ||
                  vocbase->lookupCollection(collectionName) == nullptr) {
                // 1.) re-create collection, means: overwrite=true (rw database)
                // OR 2.) not existing, new collection (rw database)
                if (!exec.isAdminUser() &&
                    !exec.canUseDatabase(dbName, auth::Level::RW)) {
                  return Result(TRI_ERROR_FORBIDDEN);
                }
              } else {
                // 3.) Existing collection (ro database, rw collection)
                // no overwrite. restoring into an existing collection
                if (!exec.isAdminUser() &&
                    !exec.canUseCollection(collectionName, auth::Level::RW)) {
                  return Result(TRI_ERROR_FORBIDDEN);
                }
              }
            } else {
              return Result(TRI_ERROR_HTTP_BAD_PARAMETER,
                            "empty collection name");
            }
          } else {
            return Result(TRI_ERROR_HTTP_BAD_PARAMETER,
                          "invalid collection name type");
          }
        } else {
          return Result(TRI_ERROR_HTTP_BAD_PARAMETER,
                        "invalid collection parameter type");
        }
      }
    }
  }
  return Result(TRI_ERROR_NO_ERROR);
}

/// @brief returns the short id of the server which should handle this request
ResultT<std::pair<std::string, bool>>
RestReplicationHandler::forwardingTarget() {
  auto base = RestVocbaseBaseHandler::forwardingTarget();
  if (base.ok() && !std::get<0>(base.get()).empty()) {
    return base;
  }

  auto res = testPermissions();
  if (!res.ok()) {
    return res;
  }

  ServerID const& DBserver = _request->value("DBserver");
  if (!DBserver.empty()) {
    // if DBserver property present, remove auth header
    return std::make_pair(DBserver, true);
  }

  return {std::make_pair(StaticStrings::Empty, false)};
}

void RestReplicationHandler::handleCommandMakeFollower() {
  bool isGlobal = false;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  bool success = false;
  VPackSlice body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }

  std::string databaseName;

  if (!isGlobal) {
    databaseName = _vocbase.name();
  }

  ReplicationApplierConfiguration configuration =
      ReplicationApplierConfiguration::fromVelocyPack(_vocbase.server(), body,
                                                      databaseName);
  configuration._skipCreateDrop = false;

  // will throw if invalid
  configuration.validate();

  // allow access to _users if appropriate
  ExecContextSuperuserScope escope(ExecContext::current().isAdminUser());

  // forget about any existing replication applier configuration
  applier->forget();
  applier->reconfigure(configuration);
  applier->startReplication();

  while (applier->isInitializing()) {  // wait for initial sync
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (_vocbase.server().isStopping()) {
      generateError(Result(TRI_ERROR_SHUTTING_DOWN));
      return;
    }
  }
  // applier->startTailing(lastLogTick, true, barrierId);

  VPackBuilder result;
  result.openObject();
  applier->toVelocyPack(result);
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief handle an unforwarded command in the coordinator case
/// If the request is well-formed and has the DBserver set, then the request
/// should already be forwarded by other means. We should only get here if
/// the request is null or the DBserver parameter is missing. This method
/// now just does a bit of error handling.
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleUnforwardedTrampolineCoordinator() {
  if (_request == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "invalid request");
  }

  // First check the DBserver component of the body json:
  ServerID DBserver = _request->value("DBserver");

  if (DBserver.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "need \"DBserver\" parameter");
    return;
  }

  TRI_ASSERT(false);  // should only get here if request is not well-formed
}

void RestReplicationHandler::handleCommandClusterInventory() {
  auto& replicationFeature = _vocbase.server().getFeature<ReplicationFeature>();
  replicationFeature.trackInventoryRequest();

  std::string const& dbName = _request->databaseName();
  bool includeSystem = _request->parsedValue("includeSystem", true);

  ClusterInfo& ci = server().getFeature<ClusterFeature>().clusterInfo();
  std::vector<std::shared_ptr<LogicalCollection>> cols =
      ci.getCollections(dbName);
  VPackBuilder resultBuilder;
  resultBuilder.openObject();

  DatabaseFeature& databaseFeature =
      _vocbase.server().getFeature<DatabaseFeature>();
  auto vocbase = databaseFeature.useDatabase(dbName);
  if (!vocbase) {
    generateError(ResponseCode::NOT_FOUND, TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    return;
  }

  resultBuilder.add(VPackValue(StaticStrings::Properties));
  vocbase->toVelocyPack(resultBuilder);
  vocbase.reset();

  auto& exec = ExecContext::current();
  ExecContextSuperuserScope escope(exec.isAdminUser());

  resultBuilder.add("collections", VPackValue(VPackValueType::Array));
  for (std::shared_ptr<LogicalCollection> const& c : cols) {
    if (!exec.isAdminUser() &&
        !exec.canUseCollection(dbName, c->name(), auth::Level::RO)) {
      continue;
    }

    // We want to check if the collection is usable and all followers
    // are in sync:
    std::shared_ptr<ShardMap> shardMap = c->shardIds();

    // shardMap is an unordered_map from ShardId (string) to a vector of
    // servers (strings), wrapped in a shared_ptr
    auto cic = ci.getCollectionCurrent(dbName,
                                       basics::StringUtils::itoa(c->id().id()));
    // Check all shards:
    bool isReady = true;
    bool allInSync = true;
    for (auto const& p : *shardMap) {
      auto currentServerList = cic->servers(p.first);
      if (c->isSmart() && c->type() == TRI_COL_TYPE_EDGE && c->isAStub()) {
        // Means we do have a Virtual SmartEdge Collection.
        // A Virtual SmartEdge Collection does not include any shards.
        // Therefore, we cannot, and we do not need to verify if shards are in
        // sync or not.
        ADB_PROD_ASSERT(!c->isSmartChild());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
        // Additionally, whenever we see a Virtual Edge Collection, we must make
        // sure that the related shadow collections are part of the inventory.
        for (auto const& shadowCollectionName : c->realNames()) {
          bool foundShadowCollection = false;
          for (auto const& logicalCollection : cols) {
            if (logicalCollection->name() == shadowCollectionName) {
              // A ShadowCollection must be a SmartChild
              TRI_ASSERT(logicalCollection->isSmartChild());
              foundShadowCollection = true;
            }
          }
          TRI_ASSERT(foundShadowCollection);
        }
#endif
      } else {
        if (currentServerList.size() == 0 || p.second.size() == 0 ||
            currentServerList[0] != p.second[0] ||
            (!p.second[0].empty() && p.second[0][0] == '_')) {
          isReady = false;
        }
        if (!ClusterHelpers::compareServerLists(p.second, currentServerList)) {
          allInSync = false;
        }
      }
    }
    c->toVelocyPackForClusterInventory(resultBuilder, includeSystem, isReady,
                                       allInSync);
  }
  resultBuilder.close();  // collections
  resultBuilder.add("views", VPackValue(VPackValueType::Array));
  LogicalView::enumerate(
      _vocbase, [&resultBuilder](LogicalView::ptr const& view) -> bool {
        if (view) {
          resultBuilder.openObject();
          auto res = view->properties(
              resultBuilder, LogicalDataSource::Serialization::Inventory);
          if (res.fail()) {
            THROW_ARANGO_EXCEPTION(res);
          }
          // details, !forPersistence because on restore any
          // datasource ids will differ, so need an end-user
          // representation
          resultBuilder.close();
        }

        return true;
      });
  resultBuilder.close();  // views

  TRI_voc_tick_t tick = TRI_CurrentTickServer();
  resultBuilder.add("tick", VPackValue(std::to_string(tick)));
  resultBuilder.add("state", VPackValue("unused"));
  resultBuilder.close();  // base
  generateResult(rest::ResponseCode::OK, resultBuilder.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the structure of a collection TODO MOVE
////////////////////////////////////////////////////////////////////////////////

futures::Future<futures::Unit>
RestReplicationHandler::handleCommandRestoreCollection() {
  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {  // error message generated in parseVPackBody
    co_return;
  }
  auto pair = rocksutils::stripObjectIds(body);
  VPackSlice const slice = pair.first;

  bool overwrite = _request->parsedValue<bool>("overwrite", false);
  bool force = _request->parsedValue<bool>("force", false);
  bool ignoreDistributeShardsLikeErrors =
      _request->parsedValue<bool>("ignoreDistributeShardsLikeErrors", false);

  Result res = co_await processRestoreCollection(
      slice, overwrite, force, ignoreDistributeShardsLikeErrors);

  if (res.is(TRI_ERROR_ARANGO_DUPLICATE_NAME)) {
    VPackSlice name = slice.get("parameters");
    if (name.isObject()) {
      name = name.get("name");
      if (name.isString() && !name.copyString().empty() &&
          name.copyString()[0] == '_') {
        // system collection, may have been created in the meantime by a
        // background action collection should be there now
        res.reset();
      }
    }
  }

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  VPackBuilder result;
  result.add(VPackValue(VPackValueType::Object));
  result.add("result", VPackValue(true));
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the indexes of a collection
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandRestoreIndexes() {
  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {  // error message generated in parseVPackBody
    return;
  }

  bool force = _request->parsedValue("force", false);

  Result res;
  if (ServerState::instance()->isCoordinator()) {
    res = processRestoreIndexesCoordinator(body, force);
  } else {
    res = processRestoreIndexes(body, force);
  }

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  VPackBuilder result;
  result.openObject();
  result.add("result", VPackValue(true));
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the data of a collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<futures::Unit>
RestReplicationHandler::handleCommandRestoreData() {
  std::string const& colName = _request->value("collection");

  if (colName.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid collection parameter, not given");
    co_return;
  }

  Result res = co_await processRestoreData(colName);

  if (res.fail()) {
    if (res.errorMessage().empty()) {
      generateError(GeneralResponse::responseCode(res.errorNumber()),
                    res.errorNumber());
    } else {
      generateError(res);
    }
  } else {
    VPackBuilder result;
    result.add(VPackValue(VPackValueType::Object));
    result.add("result", VPackValue(true));
    result.close();
    generateResult(rest::ResponseCode::OK, result.slice());
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the structure of a collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<Result> RestReplicationHandler::processRestoreCollection(
    VPackSlice const& collection, bool dropExisting, bool force,
    bool ignoreDistributeShardsLikeErrors) {
  if (!collection.isObject()) {
    co_return Result(TRI_ERROR_HTTP_BAD_PARAMETER,
                     "collection declaration is invalid");
  }

  VPackSlice const parameters = collection.get("parameters");

  {
    // NOTE: This code is only to check if attribute is there
    // TODO: Reorganize, processRestore should be called with
    // Parameters only, and restore Indexes with indexes
    // only, test of existence should be outside.
    VPackSlice const indexes = collection.get("indexes");

    if (!indexes.isArray()) {
      co_return Result(TRI_ERROR_HTTP_BAD_PARAMETER,
                       "collection indexes declaration is invalid");
    }
  }

  auto config = _vocbase.getDatabaseConfiguration();

  // Original
  auto input = CreateCollectionBody::fromRestoreAPIBody(parameters, config);
  if (input.fail()) {
    co_return input.result();
  }
  OperationOptions options(_context);

  if (ignoreHiddenEnterpriseCollection(input->name, force)) {
    co_return {TRI_ERROR_NO_ERROR};
  }

  if (arangodb::basics::VelocyPackHelper::getBooleanValue(parameters, "deleted",
                                                          false)) {
    // we don't care about deleted collections
    co_return Result();
  }

  {
    auto result = co_await handlingOfExistingCollection(_vocbase, input->name,
                                                        dropExisting);
    if (result.fail()) {
      co_return result.result();
    }
    if (result.get()) {
      // Collection still exists.
      // No point in trying to recreate it, it will fail with duplicate name.
      if (dropExisting) {
        // Overwrite mode, we have successfully truncated the collection
        // Consider this process successful.
        co_return {TRI_ERROR_NO_ERROR};
      } else {
        co_return Result(
            TRI_ERROR_ARANGO_DUPLICATE_NAME,
            std::string("duplicate collection name '") + input->name + "'");
      }
    }
  }

  // We always wait for Collections to be synced on shards
  bool waitForSyncReplication = true;
  bool isNewDatabase = false;
  bool isRestore = true;

  bool allowEnterpriseCollectionsOnSingleServer = false;
  bool enforceReplicationFactor = true;

  if (input->isSmart || input->isSatellite()) {
    allowEnterpriseCollectionsOnSingleServer = true;
  }

  std::vector<CreateCollectionBody> collections{std::move(input.get())};
  auto result = methods::Collections::create(
      _vocbase, options, collections, waitForSyncReplication,
      enforceReplicationFactor, isNewDatabase,
      allowEnterpriseCollectionsOnSingleServer, isRestore);

  co_return result.result();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the data of a collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<Result> RestReplicationHandler::processRestoreData(
    std::string const& colName) {
#ifdef USE_ENTERPRISE
  bool force = _request->parsedValue("force", false);
  if (ignoreHiddenEnterpriseCollection(colName, force)) {
    co_return {TRI_ERROR_NO_ERROR};
  }
#endif

  ExecContextSuperuserScope escope(
      ExecContext::current().isSuperuser() ||
      (ExecContext::current().isAdminUser() && !ServerState::readOnly()));

  if (colName == StaticStrings::UsersCollection) {
    // We need to handle the _users in a special way
    co_return co_await processRestoreUsersBatch();
  }

  if (colName == StaticStrings::AnalyzersCollection &&
      ServerState::instance()->isCoordinator() &&
      _vocbase.server().hasFeature<iresearch::IResearchAnalyzerFeature>()) {
    // _analyzers should be inserted via analyzers API
    co_return co_await processRestoreCoordinatorAnalyzersBatch();
  }

  auto origin = transaction::OperationOriginREST{"restoring data"};
  auto ctx = transaction::StandaloneContext::create(_vocbase, origin);
  SingleCollectionTransaction trx(std::move(ctx), colName,
                                  AccessMode::Type::WRITE);

  Result res = co_await trx.beginAsync();

  if (!res.ok()) {
    res.reset(res.errorNumber(),
              arangodb::basics::StringUtils::concatT(
                  "unable to start transaction: ", res.errorMessage()));
    co_return res;
  }

  res = processRestoreDataBatch(trx, colName);
  res = trx.finish(res);

  // for single-server we just trigger analyzers cache reload
  // once replication updated _analyzers collection. This
  // will make all newly imported analyzers available
  if (res.ok() && colName == StaticStrings::AnalyzersCollection &&
      ServerState::instance()->isSingleServer() &&
      _vocbase.server().hasFeature<iresearch::IResearchAnalyzerFeature>()) {
    server()
        .getFeature<arangodb::iresearch::IResearchAnalyzerFeature>()
        .invalidate(_vocbase,
                    transaction::OperationOriginREST{"restoring analyzers"});
  }
  co_return res;
}

Result RestReplicationHandler::parseBatch(
    transaction::Methods& trx, std::string const& collectionName,
    VPackBuilder& documentsToInsert,
    std::unordered_set<std::string>& documentsToRemove) {
  if (_request->contentType() == ContentType::VPACK) {
    return parseBatchVPack(trx, collectionName, documentsToInsert);
  }

  if (_request->contentType() == ContentType::DUMP ||
      _request->contentType() == ContentType::TEXT ||
      _request->contentType() == ContentType::JSON) {
    return parseBatchDump(trx, collectionName, documentsToInsert,
                          documentsToRemove);
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_INTERNAL,
      absl::StrCat("invalid request type: ",
                   contentTypeToString(_request->contentType())));
}

Result RestReplicationHandler::parseBatchDump(
    transaction::Methods& trx, std::string const& collectionName,
    VPackBuilder& documentsToInsert,
    std::unordered_set<std::string>& documentsToRemove) {
  TRI_ASSERT(_request->contentType() == ContentType::DUMP ||
             _request->contentType() == ContentType::TEXT ||
             _request->contentType() == ContentType::JSON);

  bool const isUsersCollection =
      collectionName == StaticStrings::UsersCollection;

  std::string_view bodyStr = _request->rawPayload();
  char const* ptr = bodyStr.data();
  char const* end = ptr + bodyStr.size();

  VPackBuilder builder(
      &basics::VelocyPackHelper::strictRequestValidationOptions);

  // First parse and collect all markers, we assemble everything in one
  // large builder holding an array
  documentsToRemove.clear();
  documentsToInsert.clear();
  documentsToInsert.openArray();

  // no more content-encoding should be present
  TRI_ASSERT(_request->header(StaticStrings::ContentEncoding).empty());

  int line = 0;
  while (ptr < end) {
    size_t remain = end - ptr;
    std::string_view currentLine(ptr, remain);
    auto nlPos = currentLine.find('\n');
    if (nlPos != std::string_view::npos) {
      ++line;
      currentLine = std::string_view(ptr, nlPos);
    }

    if (currentLine.size() > 1) {
      // found something
      VPackSlice doc;
      TRI_replication_operation_e type = REPLICATION_INVALID;

      Result res = restoreDataParser(currentLine, collectionName, line, builder,
                                     doc, type);
      if (res.fail()) {
        if (res.is(TRI_ERROR_HTTP_CORRUPTED_JSON)) {
          using namespace std::literals::string_literals;
          res.withError([&](result::Error& err) {
            err.appendErrorMessage(
                absl::StrCat(" in message '", currentLine, "'"));
          });
        }
        return res;
      }

      TRI_ASSERT(type != REPLICATION_INVALID);

      // Put into array of all parsed markers:
      if (type == REPLICATION_MARKER_DOCUMENT) {
        documentsToInsert.openObject();

        TRI_ASSERT(doc.isObject());
        bool checkKey = true;
        bool checkRev = true;
        for (auto it : VPackObjectIterator(doc, true)) {
          // only check for "_key" attribute here if we still have to.
          // once we have seen it, it will not show up again in the same
          // document
          bool const isKey =
              checkKey && (it.key.stringView() == StaticStrings::KeyString);

          if (isKey) {
            // _key attribute

            // prevent checking for _key twice in the same document
            checkKey = false;

            if (isUsersCollection) {
              // ignore _key for _users
              continue;
            }
            if (!documentsToRemove.empty()) {
              // for any document key that we have tracked in documentsToRemove,
              // we now got a new version to insert, so we need to remove the
              // key from documentsToRemove. this is expensive, but we only have
              // to pay for it if there are REPLICATION_MARKER_REMOVE markers
              // present, which can only happen with MMFiles dumps from <= 3.6
              documentsToRemove.erase(it.value.copyString());
            }

            documentsToInsert.add(it.key);
            documentsToInsert.add(it.value);
          } else if (checkRev &&
                     (it.key.stringView() == StaticStrings::RevString)) {
            // _rev attribute

            // prevent checking for _rev twice in the same document
            checkRev = false;

            // We simply get rid of the `_rev` attribute here on the
            // coordinator. We need to create a new value but it has to be
            // unique in the shard, therefore the shard leader must create the
            // value. If multiple coordinators would create a timestamp based
            // _rev value concurrently, we could get a duplicate, which would
            // lead to a clash on the actual shard leader and can lead to
            // RocksDB conflicts or even data corruption between primary index
            // and data in the documents column family.
          } else {
            // copy key/value verbatim
            documentsToInsert.add(it.key);
            documentsToInsert.add(it.value);
          }
        }

        documentsToInsert.close();
      } else if (type == REPLICATION_MARKER_REMOVE) {
        // keep track of which documents to remove.
        // in case we add a document to remove here that is already in
        // documentsToInsert, this case will be detected later.
        TRI_ASSERT(doc.isString());
        documentsToRemove.emplace(doc.copyString());
      }
    }

    ptr += currentLine.size() + 1;
  }

  // close array
  documentsToInsert.close();

  return {};
}

Result RestReplicationHandler::parseBatchVPack(
    transaction::Methods& trx, std::string const& collectionName,
    VPackBuilder& documentsToInsert) {
  TRI_ASSERT(_request->contentType() == ContentType::VPACK);

  std::string_view payload = _request->rawPayload();
  velocypack::Options options = VPackOptions::Defaults;
  options.checkAttributeUniqueness = true;
  options.validateUtf8Strings = true;
  options.disallowExternals = true;
  options.disallowCustom = false;
  options.disallowTags = true;
  options.disallowBCD = true;
  options.unsupportedTypeBehavior = VPackOptions::FailOnUnsupportedType;

  VPackValidator validator(&options);
  validator.validate(payload.data(), payload.size());  // throws on error

  VPackSlice data(reinterpret_cast<uint8_t const*>(payload.data()));
  if (!data.isArray()) {
    return {TRI_ERROR_BAD_PARAMETER,
            "expecting array of documents for restore request"};
  }

  bool const isUsersCollection =
      collectionName == StaticStrings::UsersCollection;

  // First parse and collect all markers, we assemble everything in one
  // large builder holding an array
  documentsToInsert.clear();
  documentsToInsert.openArray();

  VPackArrayIterator it(data);
  while (it.valid()) {
    VPackSlice doc = it.value();

    documentsToInsert.openObject();

    TRI_ASSERT(doc.isObject());
    bool checkKey = true;
    bool checkRev = true;
    for (auto it : VPackObjectIterator(doc, true)) {
      // only check for "_key" attribute here if we still have to.
      // once we have seen it, it will not show up again in the same
      // document
      bool const isKey =
          checkKey && (it.key.stringView() == StaticStrings::KeyString);

      if (isKey) {
        // _key attribute

        // prevent checking for _key twice in the same document
        checkKey = false;

        if (!isUsersCollection) {
          // ignore _key for _users
          documentsToInsert.add(it.key);
          documentsToInsert.add(it.value);
        }
      } else if (checkRev &&
                 (it.key.stringView() == StaticStrings::RevString)) {
        // _rev attribute

        // prevent checking for _rev twice in the same document
        checkRev = false;

        // We simply get rid of the `_rev` attribute here on the
        // coordinator. We need to create a new value but it has to be
        // unique in the shard, therefore the shard leader must create the
        // value. If multiple coordinators would create a timestamp based
        // _rev value concurrently, we could get a duplicate, which would
        // lead to a clash on the actual shard leader and can lead to
        // RocksDB conflicts or even data corruption between primary index
        // and data in the documents column family.
      } else {
        // copy key/value verbatim
        documentsToInsert.add(it.key);
        documentsToInsert.add(it.value);
      }
    }

    documentsToInsert.close();

    ++it;  // next line
  }

  // close array
  documentsToInsert.close();

  return {};
}

futures::Future<Result> RestReplicationHandler::parseBatchForSystemCollection(
    std::string const& collectionName, VPackBuilder& documentsToInsert,
    std::unordered_set<std::string>& documentsToRemove) {
  TRI_ASSERT(documentsToInsert.isEmpty());

  // this "fake" transaction here is only needed to get access to the underlying
  // system collection. we will not write anything into the collection here
  auto origin = transaction::OperationOriginREST{"restoring documents"};
  auto ctx = transaction::StandaloneContext::create(_vocbase, origin);
  SingleCollectionTransaction trx(std::move(ctx), collectionName,
                                  AccessMode::Type::READ);

  Result res = co_await trx.beginAsync();
  if (res.ok()) {
    res = parseBatch(trx, collectionName, documentsToInsert, documentsToRemove);
  }
  // transaction will end here, without anything written
  co_return res;
}

futures::Future<Result>
RestReplicationHandler::processRestoreCoordinatorAnalyzersBatch() {
  VPackBuilder documentsToInsert;
  std::unordered_set<std::string> documentsToRemove;
  Result res = co_await parseBatchForSystemCollection(
      StaticStrings::AnalyzersCollection, documentsToInsert, documentsToRemove);

  if (res.ok() && !documentsToInsert.slice().isEmptyArray()) {
    auto& analyzersFeature =
        _vocbase.server().getFeature<iresearch::IResearchAnalyzerFeature>();
    co_return analyzersFeature.bulkEmplace(
        _vocbase, documentsToInsert.slice(),
        transaction::OperationOriginREST{"restoring analyzers"});
  }
  co_return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the data of _users collection
/// Special case, we shall allow to import in this collection,
/// however it is not sharded by key and we need to replace by name instead of
/// by key
////////////////////////////////////////////////////////////////////////////////

futures::Future<Result> RestReplicationHandler::processRestoreUsersBatch() {
  VPackBuilder documentsToInsert;
  std::unordered_set<std::string> documentsToRemove;
  Result res = co_await parseBatchForSystemCollection(
      StaticStrings::UsersCollection, documentsToInsert, documentsToRemove);
  if (res.fail()) {
    co_return res;
  }

  std::string aql(
      "FOR u IN @restored UPSERT {user: u.user} INSERT u REPLACE u "
      "INTO @@collection OPTIONS {ignoreErrors: true, silent: true, "
      "waitForSync: false, isRestore: true}");

  auto bindVars = std::make_shared<VPackBuilder>();
  bindVars->openObject();
  bindVars->add("@collection", VPackValue(StaticStrings::UsersCollection));
  bindVars->add(VPackValue("restored"));
  bindVars->add(documentsToInsert.slice());
  bindVars->close();  // bindVars

  auto origin = transaction::OperationOriginREST{"restoring users"};
  auto queryFuture = arangodb::aql::runStandaloneAqlQuery(
      _vocbase, origin, aql::QueryString(aql), std::move(bindVars));
  auto queryResult = std::move(queryFuture.waitAndGet());

  // neither agency nor dbserver should get here
  AuthenticationFeature* af = AuthenticationFeature::instance();
  TRI_ASSERT(af->userManager() != nullptr);
  if (af->userManager() != nullptr) {
    af->userManager()->triggerCacheRevalidation();
  }

  co_return queryResult.result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the data of a collection
////////////////////////////////////////////////////////////////////////////////

Result RestReplicationHandler::processRestoreDataBatch(
    transaction::Methods& trx, std::string const& collectionName) {
  // we'll build all documents to insert in this builder
  VPackBuilder documentsToInsert;
  std::unordered_set<std::string> documentsToRemove;
  Result res =
      parseBatch(trx, collectionName, documentsToInsert, documentsToRemove);
  if (res.fail()) {
    return res;
  }

  OperationOptions options(_context);
  options.silent = true;
  options.ignoreRevs = true;
  options.isRestore = true;
  options.waitForSync = false;
  options.overwriteMode = OperationOptions::OverwriteMode::Replace;
  OperationResult opRes(Result(), options);

  // only go into the remove documents case when we really have to.
  if (!documentsToRemove.empty()) {
    VPackBuilder toRemove;
    toRemove.openArray();
    for (auto const& it : documentsToRemove) {
      toRemove.openObject();
      toRemove.add(StaticStrings::KeyString, VPackValue(it));
      toRemove.close();
    }
    toRemove.close();
    // we let failing removes succeed here. in case a document is not found upon
    // remove, the end result will be the same (document does not exist). in
    // addition, the removal case  is an edge case for old dumps from MMFiles
    // from <= 3.6
    trx.remove(collectionName, toRemove.slice(), options);

    // if we have pending removes we need to rewrite the entire
    // documentsToInsert array. this is expensive, but an absolute edge case as
    // mentioned before
    documentsToInsert = VPackCollection::filter(
        documentsToInsert.slice(),
        [&documentsToRemove](VPackSlice const& current, VPackValueLength) {
          TRI_ASSERT(current.isObject());
          VPackSlice key = current.get(StaticStrings::KeyString);
          return key.isString() && documentsToRemove.find(key.copyString()) ==
                                       documentsToRemove.end();
        });
  }

  // Now try to insert all keys for which the last marker was a document marker
  VPackSlice requestSlice = documentsToInsert.slice();

  try {
    opRes = trx.insert(collectionName, requestSlice, options);
    if (opRes.fail()) {
      LOG_TOPIC("e6280", WARN, Logger::CLUSTER)
          << "could not insert " << requestSlice.length()
          << " documents for restore: " << opRes.result.errorMessage();
      return opRes.result;
    }

    if (opRes.countErrorCodes.empty()) {
      // no detailed errors reported. all good
      return Result();
    }

    // at least one error occurred
    if (opRes.slice().isArray()) {
      // Now go through the individual results and check each errors
      VPackArrayIterator itRequest(requestSlice);
      VPackArrayIterator itResult(opRes.slice());

      while (itRequest.valid()) {
        VPackSlice result = *itResult;
        VPackSlice error = result.get(StaticStrings::Error);
        if (error.isTrue()) {
          error = result.get(StaticStrings::ErrorNum);
          if (error.isNumber()) {
            auto code = ErrorCode{error.getNumericValue<int>()};
            error = result.get(StaticStrings::ErrorMessage);
            if (error.isString()) {
              return {code, error.copyString()};
            }
            return {code};
          }
        }
        itRequest.next();
        itResult.next();
      }
    }

    // if we get here, we didn't have a detailed array with results.
    // so we need to stick to the error code map, which is all we got
    TRI_ASSERT(!opRes.countErrorCodes.empty());
    ErrorCode ec = (*opRes.countErrorCodes.begin()).first;
    return {ec};
  } catch (arangodb::basics::Exception const& ex) {
    LOG_TOPIC("8e8e1", WARN, Logger::CLUSTER)
        << "could not insert documents for restore exception: " << ex.what();
    return Result(ex.code(), ex.what());
  } catch (std::exception const& ex) {
    LOG_TOPIC("f1e7f", WARN, Logger::CLUSTER)
        << "could not insert documents for restore exception: " << ex.what();
    return Result(TRI_ERROR_INTERNAL, ex.what());
  } catch (...) {
    LOG_TOPIC("368bb", WARN, Logger::CLUSTER)
        << "could not insert documents for restore exception.";
    return Result(TRI_ERROR_INTERNAL);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the indexes of a collection
////////////////////////////////////////////////////////////////////////////////

Result RestReplicationHandler::processRestoreIndexes(
    VPackSlice const& collection, bool force) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  if (!collection.isObject()) {
    std::string errorMsg = "collection declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  VPackSlice const parameters = collection.get("parameters");

  if (!parameters.isObject()) {
    std::string errorMsg = "collection parameters declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  VPackSlice const indexes = collection.get("indexes");

  if (!indexes.isArray()) {
    std::string errorMsg = "collection indexes declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  VPackValueLength const n = indexes.length();

  if (n == 0) {
    // nothing to do
    return TRI_ERROR_NO_ERROR;
  }

  std::string const name = arangodb::basics::VelocyPackHelper::getStringValue(
      parameters, "name", "");

  if (name.empty()) {
    std::string errorMsg = "collection name is missing";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  if (arangodb::basics::VelocyPackHelper::getBooleanValue(parameters, "deleted",
                                                          false)) {
    // we don't care about deleted collections
    return {};
  }

  std::shared_ptr<LogicalCollection> coll = _vocbase.lookupCollection(name);
  if (!coll) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  // may be used for rebuilding index infos
  VPackBuilder rebuilder;

  Result fres;

  ExecContextSuperuserScope escope(
      ExecContext::current().isSuperuser() ||
      (ExecContext::current().isAdminUser() && !ServerState::readOnly()));

  READ_LOCKER(readLocker, _vocbase._inventoryLock);

  // look up the collection
  try {
    auto physical = coll->getPhysical();
    TRI_ASSERT(physical != nullptr);

    for (VPackSlice idxDef : VPackArrayIterator(indexes)) {
      // {"id":"229907440927234","type":"hash","unique":false,"fields":["x","Y"]}
      VPackSlice type = idxDef.get(StaticStrings::IndexType);

      if (!type.isString()) {
        continue;
      }

      if (type.isEqualString(StaticStrings::IndexNamePrimary) ||
          type.isEqualString(StaticStrings::IndexNameEdge)) {
        continue;
      }

      if (type.isEqualString("geo1") || type.isEqualString("geo2")) {
        // transform type "geo1" or "geo2" into "geo".
        rebuilder.clear();
        rebuilder.openObject();
        rebuilder.add(StaticStrings::IndexType, VPackValue("geo"));
        for (auto const& it : VPackObjectIterator(idxDef)) {
          if (!it.key.isEqualString(StaticStrings::IndexType)) {
            rebuilder.add(it.key);
            rebuilder.add(it.value);
          }
        }
        rebuilder.close();
        idxDef = rebuilder.slice();
      }

      std::shared_ptr<arangodb::Index> idx;
      try {
        bool created = false;
        idx = physical->createIndex(idxDef, /*restore*/ true, created)
                  .waitAndGet();
      } catch (basics::Exception const& e) {
        if (e.code() == TRI_ERROR_NOT_IMPLEMENTED) {
          continue;
        }

        std::string errorMsg = "could not create index: " + e.message();
        fres.reset(e.code(), errorMsg);
        break;
      }
      TRI_ASSERT(idx != nullptr);
    }
  } catch (arangodb::basics::Exception const& ex) {
    // fix error handling
    std::string errorMsg =
        "could not create index: " + std::string(TRI_errno_string(ex.code()));
    fres.reset(ex.code(), errorMsg);
  } catch (...) {
    std::string errorMsg = "could not create index: unknown error";
    fres.reset(TRI_ERROR_INTERNAL, errorMsg);
  }

  return fres;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the indexes of a collection, coordinator case
////////////////////////////////////////////////////////////////////////////////

Result RestReplicationHandler::processRestoreIndexesCoordinator(
    VPackSlice const& collection, bool force) {
  if (!collection.isObject()) {
    std::string errorMsg = "collection declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }
  VPackSlice const parameters = collection.get("parameters");

  if (!parameters.isObject()) {
    std::string errorMsg = "collection parameters declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  VPackSlice indexes = collection.get("indexes");

  if (!indexes.isArray()) {
    std::string errorMsg = "collection indexes declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  size_t const n = static_cast<size_t>(indexes.length());

  if (n == 0) {
    // nothing to do
    return {};
  }

  std::string name = arangodb::basics::VelocyPackHelper::getStringValue(
      parameters, "name", "");

  if (name.empty()) {
    std::string errorMsg = "collection indexes declaration is invalid";
    return {TRI_ERROR_HTTP_BAD_PARAMETER, errorMsg};
  }

  if (ignoreHiddenEnterpriseCollection(name, force)) {
    return {};
  }

  if (arangodb::basics::VelocyPackHelper::getBooleanValue(parameters, "deleted",
                                                          false)) {
    // we don't care about deleted collections
    return {};
  }

  auto& dbName = _vocbase.name();

  // in a cluster, we only look up by name:
  ClusterInfo& ci = server().getFeature<ClusterFeature>().clusterInfo();
  std::shared_ptr<LogicalCollection> col = ci.getCollectionNT(dbName, name);
  if (col == nullptr) {
    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
            ClusterInfo::getCollectionNotFoundMsg(dbName, name)};
  }

  TRI_ASSERT(col != nullptr);

  auto& cluster = _vocbase.server().getFeature<ClusterFeature>();

  // may be used for rebuilding index infos
  VPackBuilder rebuilder;

  Result res;

  for (VPackSlice idxDef : VPackArrayIterator(indexes)) {
    VPackSlice type = idxDef.get(StaticStrings::IndexType);

    if (!type.isString()) {
      // invalid type, cannot handle it
      continue;
    }

    if (type.isEqualString(StaticStrings::IndexNamePrimary) ||
        type.isEqualString(StaticStrings::IndexNameEdge)) {
      // must ignore these types of indexes during restore
      continue;
    }

    if (type.isEqualString("geo1") || type.isEqualString("geo2")) {
      // transform type "geo1" or "geo2" into "geo".
      rebuilder.clear();
      rebuilder.openObject();
      rebuilder.add(StaticStrings::IndexType, VPackValue("geo"));
      for (auto const& it : VPackObjectIterator(idxDef)) {
        if (!it.key.isEqualString(StaticStrings::IndexType)) {
          rebuilder.add(it.key);
          rebuilder.add(it.value);
        }
      }
      rebuilder.close();
      idxDef = rebuilder.slice();
    }

    if (type.isEqualString("fulltext")) {
      VPackSlice minLength = idxDef.get("minLength");
      if (minLength.isNumber()) {
        int length = minLength.getNumericValue<int>();
        if (length <= 0) {
          rebuilder.clear();
          rebuilder.openObject();
          rebuilder.add("minLength", VPackValue(1));
          for (auto const& it : VPackObjectIterator(idxDef)) {
            if (!it.key.isEqualString("minLength")) {
              rebuilder.add(it.key);
              rebuilder.add(it.value);
            }
          }
          rebuilder.close();
          idxDef = rebuilder.slice();
        }
      }
    }

    VPackBuilder tmp;

    res = ClusterIndexMethods::ensureIndexCoordinator(
        *col, idxDef, true, tmp, cluster.indexCreationTimeout());

    if (res.fail()) {
      return res.reset(
          res.errorNumber(),
          StringUtils::concatT("could not create index: ", res.errorMessage()));
    }
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief restores the views
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandRestoreView() {
  bool parseSuccess = false;
  VPackSlice slice = this->parseVPackBody(parseSuccess);

  if (!parseSuccess) {
    return;  // error message generated in parseVPackBody
  }

  if (!slice.isObject()) {
    generateError(ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER);
    return;
  }

  bool overwrite = _request->parsedValue<bool>("overwrite", false);
  auto nameSlice = slice.get(StaticStrings::DataSourceName);
  auto typeSlice = slice.get(StaticStrings::DataSourceType);

  if (!nameSlice.isString() || !typeSlice.isString()) {
    generateError(ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER);
    return;
  }

  LOG_TOPIC("f874e", TRACE, Logger::REPLICATION)
      << "restoring view: " << nameSlice.stringView();

  try {
    CollectionNameResolver resolver(_vocbase);
    auto view = resolver.getView(nameSlice.copyString());

    if (view) {
      if (!overwrite) {
        generateError(
            GeneralResponse::responseCode(TRI_ERROR_ARANGO_DUPLICATE_NAME),
            TRI_ERROR_ARANGO_DUPLICATE_NAME,
            StringUtils::concatT(
                "unable to restore view '", nameSlice.copyString(), ": ",
                TRI_errno_string(TRI_ERROR_ARANGO_DUPLICATE_NAME)));
        return;
      }

      auto res = view->drop();

      if (!res.ok()) {
        generateError(res);
        return;
      }
    }

    // must create() since view was drop()ed
    auto res = LogicalView::create(view, _vocbase, slice, true);

    if (!res.ok()) {
      generateError(res);

      return;
    }

    if (view == nullptr) {
      generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL,
                    "problem creating view");

      return;
    }
  } catch (basics::Exception const& ex) {
    generateError(GeneralResponse::responseCode(ex.code()), ex.code(),
                  ex.message());

    return;
  } catch (...) {
    generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL,
                  "problem creating view");

    return;
  }

  VPackBuilder result;

  result.openObject();
  result.add("result", VPackValue(true));
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

void RestReplicationHandler::handleCommandServerId() {
  VPackBuilder result;
  result.add(VPackValue(VPackValueType::Object));
  std::string const serverId = StringUtils::itoa(ServerIdFeature::getId().id());
  result.add("serverId", VPackValue(serverId));
  result.close();
  generateResult(rest::ResponseCode::OK, result.slice());
}

void RestReplicationHandler::handleCommandSync() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  bool success = false;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }

  std::string const endpoint =
      VelocyPackHelper::getStringValue(body, "endpoint", "");
  if (endpoint.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "<endpoint> must be a valid endpoint");
    return;
  }

  std::string dbname = isGlobal ? "" : _vocbase.name();
  auto config = ReplicationApplierConfiguration::fromVelocyPack(
      _vocbase.server(), body, dbname);

  // will throw if invalid
  config.validate();

  std::shared_ptr<InitialSyncer> syncer;

  if (isGlobal) {
    syncer = GlobalInitialSyncer::create(config);
  } else {
    syncer = DatabaseInitialSyncer::create(_vocbase, config);
  }

  Result r = syncer->run(config._incremental);

  if (r.fail()) {
    LOG_TOPIC("c4818", ERR, Logger::REPLICATION)
        << "failed to sync: " << r.errorMessage();
    generateError(r);
    return;
  }

  // FIXME: change response for databases
  VPackBuilder result;
  result.add(VPackValue(VPackValueType::Object));
  result.add("collections", VPackValue(VPackValueType::Array));
  for (auto const& it : syncer->getProcessedCollections()) {
    std::string const cidString = StringUtils::itoa(it.first.id());
    // Insert a collection
    result.add(VPackValue(VPackValueType::Object));
    result.add("id", VPackValue(cidString));
    result.add("name", VPackValue(it.second));
    result.close();  // one collection
  }
  result.close();  // collections

  auto tickString = std::to_string(syncer->getLastLogTick());
  result.add("lastLogTick", VPackValue(tickString));

  result.close();  // base
  generateResult(rest::ResponseCode::OK, result.slice());
}

void RestReplicationHandler::handleCommandApplierGetConfig() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  auto configuration = applier->configuration();
  VPackBuilder builder;
  builder.openObject();
  configuration.toVelocyPack(builder, false, false);
  builder.close();

  generateResult(rest::ResponseCode::OK, builder.slice());
}

void RestReplicationHandler::handleCommandApplierSetConfig() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  bool success = false;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }

  std::string databaseName;

  if (!isGlobal) {
    databaseName = _vocbase.name();
  }

  auto config = ReplicationApplierConfiguration::fromVelocyPack(
      applier->configuration(), body, databaseName);
  // will throw if invalid
  config.validate();

  applier->reconfigure(config);
  handleCommandApplierGetConfig();
}

void RestReplicationHandler::handleCommandApplierStart() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  bool found;
  std::string const& value1 = _request->value("from", found);

  TRI_voc_tick_t initialTick = 0;
  bool useTick = false;

  if (found) {
    // query parameter "from" specified
    initialTick = static_cast<TRI_voc_tick_t>(StringUtils::uint64(value1));
    useTick = true;
  }

  applier->startTailing(initialTick, useTick);
  handleCommandApplierGetState();
}

void RestReplicationHandler::handleCommandApplierStop() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  applier->stopAndJoin();
  handleCommandApplierGetState();
}

void RestReplicationHandler::handleCommandApplierGetState() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  VPackBuilder builder;
  builder.openObject();
  applier->toVelocyPack(builder);
  builder.close();
  generateResult(rest::ResponseCode::OK, builder.slice());
}

void RestReplicationHandler::handleCommandApplierGetStateAll() {
  if (_request->databaseName() != StaticStrings::SystemDatabase) {
    generateError(
        rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN,
        "global inventory can only be fetched from within _system database");
    return;
  }
  DatabaseFeature& databaseFeature =
      _vocbase.server().getFeature<DatabaseFeature>();

  VPackBuilder builder;
  builder.openObject();
  for (auto& name : databaseFeature.getDatabaseNames()) {
    auto vocbase = databaseFeature.useDatabase(name);

    if (vocbase == nullptr) {
      continue;
    }

    ReplicationApplier* applier = vocbase->replicationApplier();

    if (applier == nullptr) {
      continue;
    }

    builder.add(name, VPackValue(VPackValueType::Object));
    applier->toVelocyPack(builder);
    builder.close();
  }
  builder.close();

  generateResult(rest::ResponseCode::OK, builder.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief delete the state of the replication applier
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandApplierDeleteState() {
  bool isGlobal;
  ReplicationApplier* applier = getApplier(isGlobal);
  if (applier == nullptr) {
    return;
  }

  applier->forget();

  handleCommandApplierGetState();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief add a follower of a shard to the list of followers
////////////////////////////////////////////////////////////////////////////////

futures::Future<futures::Unit>
RestReplicationHandler::handleCommandAddFollower() {
  TRI_ASSERT(ServerState::instance()->isDBServer());

  bool success = false;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    co_return;
  }
  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an object with attributes 'followerId' "
                  "and 'shard'");
    co_return;
  }
  VPackSlice const followerIdSlice = body.get("followerId");
  VPackSlice const readLockIdSlice = body.get("readLockId");
  VPackSlice const shardSlice = body.get("shard");
  VPackSlice const checksumSlice = body.get("checksum");
  if (!followerIdSlice.isString() || !shardSlice.isString() ||
      !checksumSlice.isString()) {
    generateError(
        rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
        "'followerId', 'shard' and 'checksum' attributes must be strings");
    co_return;
  }

  auto col = _vocbase.lookupCollection(shardSlice.stringView());
  if (col == nullptr || col->deleted()) {
    generateError(Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
    co_return;
  }

  std::string const followerId = followerIdSlice.copyString();
  LOG_TOPIC("312cc", DEBUG, Logger::REPLICATION)
      << "Attempt to Add Follower: " << followerId << " to shard "
      << col->name() << " in database: " << _vocbase.name();
  // Short cut for the case that the collection is empty
  if (readLockIdSlice.isNone()) {
    LOG_TOPIC("aaff2", DEBUG, Logger::REPLICATION)
        << "Try add follower fast-path (no documents)";
    auto origin = transaction::OperationOriginInternal{"adding follower"};
    auto ctx = transaction::StandaloneContext::create(_vocbase, origin);
    SingleCollectionTransaction trx(std::move(ctx), *col,
                                    AccessMode::Type::EXCLUSIVE);
    auto res = co_await trx.beginAsync();

    if (res.ok()) {
      OperationOptions options(_context);
      auto countRes =
          trx.count(col->name(), transaction::CountType::kNormal, options);

      if (countRes.ok()) {
        VPackSlice nrSlice = countRes.slice();
        uint64_t nr = nrSlice.getNumber<uint64_t>();
        LOG_TOPIC("533c3", DEBUG, Logger::REPLICATION)
            << "Compare with shortcut Leader: " << nr
            << " == Follower: " << checksumSlice.stringView();
        if (nr == 0 && checksumSlice.isEqualString("0")) {
          res = col->followers()->add(followerId);

          if (res.fail()) {
            // this will create an error response with the appropriate message
            THROW_ARANGO_EXCEPTION(res);
          }

          VPackBuilder b;
          {
            VPackObjectBuilder bb(&b);
            b.add(StaticStrings::Error, VPackValue(false));
          }

          generateResult(rest::ResponseCode::OK, b.slice());
          LOG_TOPIC("c316e", DEBUG, Logger::REPLICATION)
              << followerId << " is now following on shard " << _vocbase.name()
              << "/" << col->name();
          co_return;
        }
      }
    }
    // If we get here, we have to report an error:
    generateError(rest::ResponseCode::FORBIDDEN,
                  TRI_ERROR_REPLICATION_SHARD_NONEMPTY, "shard not empty");
    LOG_TOPIC("8bf6e", DEBUG, Logger::REPLICATION)
        << followerId << " is not yet in sync with " << _vocbase.name() << "/"
        << col->name();
    co_return;
  }

  if (!readLockIdSlice.isString() || readLockIdSlice.getStringLength() == 0) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "'readLockId' is not a string or empty");
    co_return;
  }

  auto cleanup = scopeGuard([&body, this]() noexcept {
    try {
      // untrack the (async) replication client, so the WAL may be cleaned
      arangodb::ServerId const serverId{StringUtils::uint64(
          basics::VelocyPackHelper::getStringValue(body, "serverId", ""))};
      SyncerId const syncerId = SyncerId{StringUtils::uint64(
          basics::VelocyPackHelper::getStringValue(body, "syncerId", ""))};
      std::string const clientInfo =
          basics::VelocyPackHelper::getStringValue(body, "clientInfo", "");
      _vocbase.replicationClients().untrack(SyncerId{syncerId}, serverId,
                                            clientInfo);
    } catch (std::exception const& ex) {
      // not much we can do here except logging
      LOG_TOPIC("312ff", WARN, Logger::REPLICATION)
          << "unable to remove progress tracker: " << ex.what();
    }
  });

  LOG_TOPIC("23b9f", DEBUG, Logger::REPLICATION)
      << "Try add follower with documents";
  // previous versions < 3.3x might not send the checksum, if mixed clusters
  // get into trouble here we may need to be more lenient
  TRI_ASSERT(checksumSlice.isString() && readLockIdSlice.isString());

  TransactionId readLockId = ExtractReadlockId(readLockIdSlice);

  // referenceChecksum is the stringified number of documents in the collection
  ResultT<std::string> referenceChecksum =
      co_await computeCollectionChecksum(readLockId, col.get());
  if (!referenceChecksum.ok()) {
    generateError(std::move(referenceChecksum).result());
    co_return;
  }

  TRI_IF_FAILURE("LeaderWrongChecksumOnSoftLock" + col->name()) {
    // Check if we are a softLock, if yes simulate non-matching checksums.
    transaction::Manager* mgr = transaction::ManagerFeature::manager();
    TRI_ASSERT(mgr != nullptr);
    auto trxCtxtLease =
        mgr->leaseManagedTrx(readLockId, AccessMode::Type::READ, true)
            .waitAndGet();
    if (trxCtxtLease) {
      transaction::Methods trx{trxCtxtLease};
      if (!trx.isLocked(col.get(), AccessMode::Type::EXCLUSIVE)) {
        referenceChecksum =
            ResultT<std::string>::success("ThisCannotBeMatched");
      }
    }
  }

  LOG_TOPIC("40b17", DEBUG, Logger::REPLICATION)
      << "Compare Leader: " << referenceChecksum.get()
      << " == Follower: " << checksumSlice.stringView();
  if (!checksumSlice.isEqualString(referenceChecksum.get())) {
    LOG_TOPIC("94ebe", DEBUG, Logger::REPLICATION)
        << followerId << " is not yet in sync with " << _vocbase.name() << "/"
        << col->name();
    LOG_TOPIC("592ef", WARN, Logger::REPLICATION)
        << "Cannot add follower " << followerId << " for shard "
        << _vocbase.name() << "/" << col->name() << ", mismatching checksums. "
        << "Expected (leader): " << referenceChecksum.get()
        << ", actual (follower): " << checksumSlice.stringView();
    generateError(
        rest::ResponseCode::BAD, TRI_ERROR_REPLICATION_WRONG_CHECKSUM,
        absl::StrCat(
            "'checksum' is wrong. Expected (leader): ", referenceChecksum.get(),
            ". actual (follower): ", checksumSlice.stringView()));
    co_return;
  }

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
  // compare hash and count values of the local revision tree in case the
  // caller posted the revision tree data as well.
  // this check is only performed during failure-testing.
  // it is not safe to activate it in general, because revision trees on the
  // leader can advance when there are further writes into the shard.
  if (body.get("treeHash").isString() && body.get("treeCount").isString()) {
    auto origin =
        transaction::OperationOriginInternal{"validating revision tree"};
    auto context = transaction::StandaloneContext::create(_vocbase, origin);
    SingleCollectionTransaction trx(context, *col, AccessMode::Type::READ);

    auto res = co_await trx.beginAsync();
    if (res.ok()) {
      auto tree = col->getPhysical()->revisionTree(trx);
      if (std::to_string(tree->rootValue()) !=
          body.get("treeHash").stringView()) {
        generateError(
            rest::ResponseCode::BAD, TRI_ERROR_REPLICATION_WRONG_CHECKSUM,
            absl::StrCat(
                "revision tree hashes are different. Expected (leader): ",
                tree->rootValue(),
                ". actual (follower): ", body.get("treeHash").stringView()));
        co_return;
      }

      if (std::to_string(tree->count()) != body.get("treeCount").stringView()) {
        generateError(
            rest::ResponseCode::BAD, TRI_ERROR_REPLICATION_WRONG_CHECKSUM,
            absl::StrCat(
                "revision tree counts are different. Expected (leader): ",
                tree->count(),
                ". actual (follower): ", body.get("treeCount").stringView()));
        co_return;
      }
    }
  }
#endif

  Result res = col->followers()->add(followerId);

  if (res.fail()) {
    // this will create an error response with the appropriate message
    THROW_ARANGO_EXCEPTION(res);
  }

  VPackBuilder b;
  {
    VPackObjectBuilder bb(&b);
    b.add(StaticStrings::Error, VPackValue(false));
  }

  LOG_TOPIC("c13d4", DEBUG, Logger::REPLICATION)
      << followerId << " is now following on shard " << _vocbase.name() << "/"
      << col->name();
  generateResult(rest::ResponseCode::OK, b.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a follower of a shard from the list of followers
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandRemoveFollower() {
  TRI_ASSERT(ServerState::instance()->isDBServer());

  bool success = false;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }
  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an object with attributes 'followerId' "
                  "and 'shard'");
    return;
  }
  VPackSlice const followerId = body.get("followerId");
  VPackSlice const shard = body.get("shard");
  if (!followerId.isString() || !shard.isString()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "'followerId' and 'shard' attributes must be strings");
    return;
  }

  auto col = _vocbase.lookupCollection(shard.stringView());

  if (col == nullptr) {
    generateError(rest::ResponseCode::SERVER_ERROR,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  "did not find collection");
    return;
  }

  Result res = col->followers()->remove(followerId.copyString());

  if (res.fail()) {
    // this will create an error response with the appropriate message
    THROW_ARANGO_EXCEPTION(res);
  }

  VPackBuilder b;
  {
    VPackObjectBuilder bb(&b);
    b.add(StaticStrings::Error, VPackValue(false));
  }

  generateResult(rest::ResponseCode::OK, b.slice());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief update the leader of a shard
//////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandSetTheLeader() {
  TRI_ASSERT(ServerState::instance()->isDBServer());

  bool success = false;
  VPackSlice body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }
  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an object with attributes 'leaderId' "
                  "and 'shard'");
    return;
  }

  VPackSlice leaderIdSlice = body.get("leaderId");
  VPackSlice oldLeaderIdSlice = body.get("oldLeaderId");
  VPackSlice shard = body.get("shard");
  VPackSlice followingTermId = body.get(StaticStrings::FollowingTermId);
  // Note that we tolerate if followingTermId is not present or not a number
  // for upgrade scenarios. If the new leader does not send it, it will also
  // not send it in the option IsSynchronousReplication.
  if (!leaderIdSlice.isString() || !shard.isString() ||
      !oldLeaderIdSlice.isString()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "'leaderId' and 'shard' attributes must be strings");
    return;
  }

  std::string leaderId = leaderIdSlice.copyString();
  auto col = _vocbase.lookupCollection(shard.stringView());

  if (col == nullptr) {
    generateError(rest::ResponseCode::SERVER_ERROR,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                  "did not find collection");
    return;
  }

  std::string const currentLeaderCopy = col->followers()->getLeader();
  std::string currentLeader = currentLeaderCopy;
  if (currentLeader ==
      maintenance::ResignShardLeadership::LeaderNotYetKnownString) {
    // We have resigned, check that we are the old leader
    currentLeader = ServerState::instance()->getId();
  } else {
    auto pos = currentLeader.find(
        '_');  // this separates the FollowingTermId // from the actual leader
    if (pos != std::string::npos) {
      currentLeader = currentLeader.substr(0, pos);
    }
  }

  std::string newLeader;

  if (leaderId != currentLeader) {
    // leader server id change (i.e. different server has taken over)
    Result res = checkPlanLeaderDirect(col, leaderId);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    if (!oldLeaderIdSlice.isEqualString(currentLeader)) {
      LOG_TOPIC("aaee2", WARN, Logger::MAINTENANCE)
          << "SetTheLeader: Old leader not as expected: oldLeaderIdSlice: "
          << oldLeaderIdSlice.toJson() << ", currentLeader: " << currentLeader;
      generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN,
                    "old leader not as expected");
      return;
    }

    if (followingTermId.isNumber()) {
      newLeader =
          absl::StrCat(leaderId, "_", followingTermId.getNumber<uint64_t>());
    } else {
      newLeader = leaderId;
    }
  } else {
    // no leader change, but maybe the session id for the same leader has
    // changed
    if (followingTermId.isNumber() && !leaderId.empty() &&
        currentLeaderCopy !=
            maintenance::ResignShardLeadership::LeaderNotYetKnownString) {
      Result res = checkPlanLeaderDirect(col, leaderId);
      if (res.fail()) {
        THROW_ARANGO_EXCEPTION(res);
      }

      newLeader =
          absl::StrCat(leaderId, "_", followingTermId.getNumber<uint64_t>());
    }
  }

  if (!newLeader.empty()) {
    LOG_TOPIC("71b25", DEBUG, Logger::REPLICATION)
        << "setting leader for shard '" << _vocbase.name() << "/"
        << shard.stringView() << "' from " << currentLeaderCopy << " to "
        << newLeader;
    if (!col->followers()->setTheLeaderConditional(currentLeaderCopy,
                                                   newLeader)) {
      generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN,
                    "leader changed concurrently during leader change attempt");
      return;
    }
  }

  VPackBuilder b;
  {
    VPackObjectBuilder bb(&b);
    b.add(StaticStrings::Error, VPackValue(false));
  }

  generateResult(rest::ResponseCode::OK, b.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief hold a read lock on a collection to stop writes temporarily
////////////////////////////////////////////////////////////////////////////////

futures::Future<futures::Unit>
RestReplicationHandler::handleCommandHoldReadLockCollection() {
  TRI_ASSERT(ServerState::instance()->isDBServer());
  bool success = false;
  VPackSlice body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    co_return;
  }

  RebootId rebootId(0);
  std::string serverId;

  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an object with attributes 'collection', "
                  "'ttl' and 'id'");
    co_return;
  }

  VPackSlice collection = body.get("collection");
  VPackSlice ttlSlice = body.get("ttl");
  VPackSlice idSlice = body.get("id");

  if (body.hasKey(StaticStrings::RebootId)) {
    if (body.get(StaticStrings::RebootId).isInteger()) {
      if (body.get("serverId").isString()) {
        rebootId =
            RebootId(body.get(StaticStrings::RebootId).getNumber<uint64_t>());
        serverId = body.get("serverId").copyString();
      } else {
        generateError(
            rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
            "'rebootId' must be accompanied by string attribute 'serverId'");
        co_return;
      }
    } else {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "'rebootId' must be an integer attribute");
      co_return;
    }
  }

  if (!collection.isString() || !ttlSlice.isNumber() || !idSlice.isString()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "'collection' must be a string and 'ttl' a number and "
                  "'id' a string");
    co_return;
  }

  TransactionId id = ExtractReadlockId(idSlice);
  auto col = _vocbase.lookupCollection(collection.stringView());

  if (col == nullptr || col->deleted()) {
    generateError(Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
    co_return;
  }

  // 0.0 means using the default timeout (whatever that is)
  double ttl = VelocyPackHelper::getNumericValue(ttlSlice, 0.0);

  // This is an optional parameter, it may not be set (backwards compatible)
  // If it is not set it will default to a hard-lock, otherwise we do a
  // potentially faster soft-lock synchronization with a smaller hard-lock
  // phase.

  // TODO: the follower will always send "doSoftLockOnly=false" from 3.12.1
  // onwards. we can remove the entire parameter in the future.
  bool doSoftLock = VelocyPackHelper::getBooleanValue(
      body, StaticStrings::ReplicationSoftLockOnly, false);
  AccessMode::Type lockType = AccessMode::Type::READ;
  if (!doSoftLock) {
    // With not doSoftLock we trigger RocksDB to stop writes on this shard.
    // With a softLock we only stop the WAL from being collected,
    // but still allow writes.
    // This has potential to never ever finish, so we need a short
    // hard lock for the final sync.
    lockType = AccessMode::Type::EXCLUSIVE;

    TRI_IF_FAILURE("LeaderBlockRequestsLanesForSyncOnShard" + col->name()) {
      TRI_AddFailurePointDebugging("BlockSchedulerMediumQueue");
    }
  }

  LOG_TOPIC("4fac2", DEBUG, Logger::REPLICATION)
      << "Attempt to create a Lock: " << id << " for shard: " << _vocbase.name()
      << "/" << col->name() << " of type: " << (doSoftLock ? "soft" : "hard");
  Result res = co_await createBlockingTransaction(id, *col, ttl, lockType,
                                                  rebootId, serverId);
  if (!res.ok()) {
    LOG_TOPIC("5f00f", DEBUG, Logger::REPLICATION)
        << "Lock " << id << " for shard " << _vocbase.name() << "/"
        << col->name() << " of type: " << (doSoftLock ? "soft" : "hard")
        << " could not be created because of: " << res.errorMessage();
    generateError(res);
    co_return;
  }

  // check if we are not the actual leader anymore. this can
  // happen if there is a leader change after a follower scheduled a
  // synchronize shard job
  bool isLeader = col->followers()->getLeader().empty();
  if (!isLeader) {
    auto res = cancelBlockingTransaction(id);
    if (!res.ok()) {
      // this is potentially bad!
      LOG_TOPIC("957fa", WARN, Logger::REPLICATION)
          << "Lock " << id << " for shard " << _vocbase.name() << "/"
          << col->name() << " of type: " << (doSoftLock ? "soft" : "hard")
          << " could not be canceled because of: " << res.errorMessage();
    }
    // indicate that we are not the leader
    generateError(TRI_ERROR_CLUSTER_NOT_LEADER);
    co_return;
  }

  TRI_ASSERT(isLockHeld(id).ok());

  VPackBuilder b;
  {
    VPackObjectBuilder bb(&b);
    b.add(StaticStrings::Error, VPackValue(false));
    if (!serverId.empty()) {
      // check if the follower sent us the "wantFollowingTerm" attribute.
      // followers >= 3.8.3 will send this to indicate that they know how
      // to handle following term ids safely
      bool wantFollowingTerm =
          VelocyPackHelper::getBooleanValue(body, "wantFollowingTerm", false);
      if (wantFollowingTerm) {
        b.add(StaticStrings::FollowingTermId,
              VPackValue(col->followers()->newFollowingTermId(serverId)));
      } else {
        // a client < 3.8.3 does not know how to handle following term ids
        // safely. in this case we set the follower's term id to 0, so it
        // will be ignored on followers < 3.8.3
        col->followers()->setFollowingTermId(serverId, 0);
        b.add(StaticStrings::FollowingTermId, VPackValue(0));
      }
    }

    // also return the _current_ last log sequence number. this may be higher
    // than the tick of the transaction created above, but it still can be
    // used by a follower as an upper bound until which to tail the WAL _at
    // most_.
    TRI_ASSERT(server().hasFeature<EngineSelectorFeature>());
    StorageEngine& engine =
        server().getFeature<EngineSelectorFeature>().engine();
    b.add("lastLogTick", VPackValue(engine.currentTick()));
  }

  LOG_TOPIC("61a9d", DEBUG, Logger::REPLICATION)
      << "Shard: " << _vocbase.name() << "/" << col->name()
      << " is now locked with type: " << (doSoftLock ? "soft" : "hard")
      << " lock id: " << id;
  generateResult(rest::ResponseCode::OK, b.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief cancel the holding of a read lock on a collection
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandCancelHoldReadLockCollection() {
  TRI_ASSERT(ServerState::instance()->isDBServer());

  bool success = false;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }

  if (!body.isObject()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an object with attribute 'id'");
    return;
  }
  VPackSlice const idSlice = body.get("id");
  if (!idSlice.isString()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "'id' needs to be a string");
    return;
  }
  TransactionId id = ExtractReadlockId(idSlice);
  LOG_TOPIC("9a5e3", DEBUG, Logger::REPLICATION)
      << "Attempt to cancel Lock: " << id;

  auto res = cancelBlockingTransaction(id);
  if (!res.ok()) {
    LOG_TOPIC("9caf7", DEBUG, Logger::REPLICATION)
        << "Lock " << id << " not canceled because of: " << res.errorMessage();
    generateError(std::move(res).result());
    return;
  }

  VPackBuilder b;
  {
    VPackObjectBuilder bb(&b);
    b.add(StaticStrings::Error, VPackValue(false));
    b.add("lockHeld", VPackValue(res.get()));
  }

  LOG_TOPIC("a58e5", DEBUG, Logger::REPLICATION)
      << "Lock: " << id << " is now canceled, "
      << (res.get() ? "it is still in use." : "it is gone.");
  generateResult(rest::ResponseCode::OK, b.slice());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get ID for a read lock job
////////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandGetIdForReadLockCollection() {
  TRI_ASSERT(ServerState::instance()->isDBServer());

  std::string id =
      std::to_string(transaction::Context::makeTransactionId().id());
  VPackBuilder b;
  {
    VPackObjectBuilder bb(&b);
    b.add("id", VPackValue(id));
  }

  generateResult(rest::ResponseCode::OK, b.slice());
}

void RestReplicationHandler::handleCommandLoggerState() {
  TRI_ASSERT(server().hasFeature<EngineSelectorFeature>());
  StorageEngine& engine = server().getFeature<EngineSelectorFeature>().engine();

  VPackBuilder builder;
  auto res = engine.createLoggerState(&_vocbase, builder);

  if (res.fail()) {
    LOG_TOPIC("c7471", DEBUG, Logger::REPLICATION)
        << "failed to create logger-state" << res.errorMessage();
    generateError(rest::ResponseCode::BAD, res.errorNumber(),
                  res.errorMessage());
    return;
  }

  generateResult(rest::ResponseCode::OK, builder.slice());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the first tick available in a logfile
/// @route GET logger-first-tick
/// @caller js/client/modules/@arangodb/replication.js
/// @response VPackObject with minTick of LogfileManager->ranges()
//////////////////////////////////////////////////////////////////////////////
void RestReplicationHandler::handleCommandLoggerFirstTick() {
  TRI_voc_tick_t tick = UINT64_MAX;
  Result res =
      server().getFeature<EngineSelectorFeature>().engine().firstTick(tick);

  VPackBuilder b;
  b.add(VPackValue(VPackValueType::Object));
  if (tick == UINT64_MAX || res.fail()) {
    b.add("firstTick", VPackValue(VPackValueType::Null));
  } else {
    auto tickString = std::to_string(tick);
    b.add("firstTick", VPackValue(tickString));
  }
  b.close();
  generateResult(rest::ResponseCode::OK, b.slice());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the available logfile range
/// @route GET logger-tick-ranges
/// @caller js/client/modules/@arangodb/replication.js
/// @response VPackArray, containing info about each datafile
///           * filename
///           * status
///           * tickMin - tickMax
//////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandLoggerTickRanges() {
  TRI_ASSERT(server().hasFeature<EngineSelectorFeature>());
  StorageEngine& engine = server().getFeature<EngineSelectorFeature>().engine();
  VPackBuilder b;
  Result res = engine.createTickRanges(b);
  if (res.ok()) {
    generateResult(rest::ResponseCode::OK, b.slice());
  } else {
    generateError(res);
  }
}

bool RestReplicationHandler::prepareRevisionOperation(
    RevisionOperationContext& ctx) {
  LOG_TOPIC("253e2", TRACE, arangodb::Logger::REPLICATION)
      << "enter prepareRevisionOperation";

  // get collection name
  if (!prepareCollectionForRevisionOperation(ctx)) {
    return false;
  }
  TRI_ASSERT(!ctx.cname.empty());
  TRI_ASSERT(ctx.collection != nullptr);

  // get batchId
  bool found = false;
  std::string const& batchIdString = _request->value("batchId", found);
  if (found) {
    ctx.batchId = StringUtils::uint64(batchIdString);
  } else {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "replication revision tree - request misses batchId");
    return false;
  }

  // get resume.
  // we first try the parameter "resumeHLC" - if set, this will contain a
  // timestamp-encoded HLC value
  std::string const& resumeString =
      _request->value(StaticStrings::RevisionTreeResumeHLC, found);
  if (found) {
    // "resumeHLC" is set. use it
    ctx.resume = RevisionId::fromHLC(resumeString);
  } else {
    // "resumeHLC" is not set. now fall back to the parameter "resume". this
    // parameter contains either a numeric value of a timestamp-encoded HLC
    // value
    std::string const& resumeString =
        _request->value(StaticStrings::RevisionTreeResume, found);
    if (found) {
      ctx.resume = RevisionId::fromString(resumeString);
    }
  }

  // print request
  LOG_TOPIC("2b70f", TRACE, arangodb::Logger::REPLICATION)
      << "requested revision tree for collection '" << ctx.cname
      << "' using contextId '" << ctx.batchId << "'";

  ctx.iter = ctx.collection->getPhysical()->getReplicationIterator(
      ReplicationIterator::Ordering::Revision, ctx.batchId);
  if (!ctx.iter) {
    generateError(rest::ResponseCode::SERVER_ERROR, TRI_ERROR_INTERNAL,
                  "could not get replication iterator for collection '" +
                      ctx.cname + "'");
    return false;
  }

  return true;
}

bool RestReplicationHandler::prepareCollectionForRevisionOperation(
    RevisionOperationContext& ctx) {
  // get collection Name
  ctx.cname = _request->value("collection");
  if (ctx.cname.empty()) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "invalid collection parameter");
    return false;
  }

  // print request
  LOG_TOPIC("6e075", TRACE, arangodb::Logger::REPLICATION)
      << "requested revision tree for collection '" << ctx.cname << "'";

  ExecContextSuperuserScope escope(ExecContext::current().isAdminUser());

  if (!ExecContext::current().canUseCollection(_vocbase.name(), ctx.cname,
                                               auth::Level::RO)) {
    generateError(rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN);
    return false;
  }

  ctx.collection = _vocbase.lookupCollection(ctx.cname);
  if (!ctx.collection) {
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return false;
  }
  if (!ctx.collection->syncByRevision()) {
    generateError(rest::ResponseCode::NOT_IMPLEMENTED,
                  TRI_ERROR_NOT_IMPLEMENTED,
                  "this collection doesn't support revision-based replication");
    return false;
  }

  return true;
}

futures::Future<futures::Unit>
RestReplicationHandler::handleCommandRebuildRevisionTree() {
  RevisionOperationContext ctx;
  // get collection Name
  if (!prepareCollectionForRevisionOperation(ctx)) {
    // error was already generator by called function
    co_return;
  }
  TRI_ASSERT(!ctx.cname.empty());
  TRI_ASSERT(ctx.collection != nullptr);

  // increase metric
  ++server().getFeature<ClusterFeature>().syncTreeRebuildCounter();

  Result res = co_await ctx.collection->getPhysical()->rebuildRevisionTree();
  if (res.fail()) {
    generateError(res);
    co_return;
  }

  generateResult(rest::ResponseCode::NO_CONTENT, VPackSlice::nullSlice());
}

#ifdef ARANGODB_ENABLE_FAILURE_TESTS
void RestReplicationHandler::handleCommandCorruptRevisionTree() {
  RevisionOperationContext ctx;
  // get collection name
  if (!prepareCollectionForRevisionOperation(ctx)) {
    // error was already generated by called function
    return;
  }
  TRI_ASSERT(!ctx.cname.empty());
  TRI_ASSERT(ctx.collection != nullptr);

  auto count = _request->parsedValue("count", uint64_t(0));
  auto hash = _request->parsedValue("hash", uint64_t(0xbadbadbadbadULL));
  static_cast<RocksDBCollection*>(ctx.collection->getPhysical())
      ->corruptRevisionTree(count, hash);

  generateResult(rest::ResponseCode::OK, VPackSlice::nullSlice());
}
#endif

//////////////////////////////////////////////////////////////////////////////
/// @brief return the requested revision ranges for a given collection, if
///        available
/// @response VPackObject, containing
///           * ranges, VPackArray of VPackArray of revisions
///           * resume, optional, if response is chunked; revision resume
///                     point to specify on subsequent requests
//////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandRevisionRanges() {
  RevisionOperationContext ctx;
  if (!prepareRevisionOperation(ctx)) {
    return;
  }

  bool success = false;
  std::size_t current = 0;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }
  bool badFormat = !body.isArray();
  if (!badFormat) {
    std::size_t i = 0;
    RevisionId previousRight = RevisionId::none();
    for (VPackSlice entry : VPackArrayIterator(body)) {
      if (!entry.isArray() || entry.length() != 2) {
        badFormat = true;
        break;
      }
      VPackSlice first = entry.at(0);
      VPackSlice second = entry.at(1);
      if (!first.isString() || !second.isString()) {
        badFormat = true;
        break;
      }
      // note: always decoding as HLC timestamps is fine here, because the
      // sender side unconditionally encodes the revisions using HLC-encoding
      RevisionId left = RevisionId::fromHLC(first.stringView());
      RevisionId right = RevisionId::fromHLC(second.stringView());
      if (left == RevisionId::max() || right == RevisionId::max() ||
          left >= right || left < previousRight) {
        badFormat = true;
        break;
      }
      if (left <= ctx.resume || (i > 0 && previousRight < ctx.resume)) {
        current = std::max(current, i);
      }
      previousRight = right;
      ++i;
    }
  }
  if (badFormat) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an array of pairs of revisions");
    return;
  }

  bool encodeAsHLC = _request->parsedValue("encodeAsHLC", false);

  RevisionReplicationIterator& it =
      *static_cast<RevisionReplicationIterator*>(ctx.iter.get());
  it.seek(ctx.resume);

  std::size_t constexpr limit = 64 * 1024;  // ~2MB data
  std::size_t total = 0;
  VPackBuilder response;
  {
    VPackObjectBuilder obj(&response);
    RevisionId resumeNext = ctx.resume;
    VPackSlice range;
    RevisionId left;
    RevisionId right;
    auto setRange = [&body, &range, &left, &right](std::size_t index) -> void {
      range = body.at(index);
      // the sender side always encodes the revisions here using HLC encoding
      left = RevisionId::fromHLC(range.at(0).stringView());
      right = RevisionId::fromHLC(range.at(1).stringView());
    };
    setRange(current);

    char ridBuffer[arangodb::basics::maxUInt64StringSize];
    {
      TRI_ASSERT(response.isOpenObject());
      VPackArrayBuilder rangesGuard(&response,
                                    StaticStrings::RevisionTreeRanges);
      TRI_ASSERT(response.isOpenArray());
      bool subOpen = false;
      while (total < limit && current < body.length()) {
        if (!it.hasMore() || it.revision() <= left || it.revision() > right ||
            (!subOpen && it.revision() <= right)) {
          auto target = std::max(ctx.resume, left);
          if (it.hasMore() && it.revision() < target) {
            it.seek(target);
          }
          TRI_ASSERT(!subOpen);
          response.openArray();
          subOpen = true;
          resumeNext = std::max(ctx.resume.next(), left);
        }

        if (it.hasMore() && it.revision() >= left && it.revision() <= right) {
          if (encodeAsHLC) {
            response.add(VPackValue(it.revision().toHLC()));
          } else {
            response.add(it.revision().toValuePair(ridBuffer));
          }
          ++total;
          resumeNext = it.revision().next();
          it.next();
        }

        if (!it.hasMore() || it.revision() > right) {
          TRI_ASSERT(response.isOpenArray());
          TRI_ASSERT(subOpen);
          subOpen = false;
          response.close();
          TRI_ASSERT(response.isOpenArray());
          resumeNext = right.next();
          ++current;
          if (current < body.length()) {
            setRange(current);
          }
        }
      }
      if (subOpen) {
        TRI_ASSERT(response.isOpenArray());
        response.close();
        subOpen = false;
        TRI_ASSERT(response.isOpenArray());
      }
      TRI_ASSERT(!subOpen);
      TRI_ASSERT(response.isOpenArray());
      // exit scope closes "ranges"
    }

    TRI_ASSERT(response.isOpenObject());

    if (body.length() >= 1) {
      setRange(body.length() - 1);
      if (body.length() > 0 && resumeNext <= right) {
        response.add(StaticStrings::RevisionTreeResume,
                     resumeNext.toValuePair(ridBuffer));
        response.add(StaticStrings::RevisionTreeResumeHLC,
                     VPackValue(resumeNext.toHLC()));
      }
    }
  }

  generateResult(rest::ResponseCode::OK, response.slice());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief return the requested documents from a given collection, if
///        available
/// @response VPackArray, containing VPackObject documents or errors
//////////////////////////////////////////////////////////////////////////////

void RestReplicationHandler::handleCommandRevisionDocuments() {
  RevisionOperationContext ctx;
  if (!prepareRevisionOperation(ctx)) {
    return;
  }

  // the "encodeAsHLC" parameter is sent from the following versions onwards:
  // - 3.8.8 or higher
  // - 3.9.4 or higher
  // - 3.10.1 or higher
  // - 3.11.0 or higher
  // - 4.0 higher
  bool encodeAsHLC = _request->parsedValue("encodeAsHLC", false);

  bool success = false;
  VPackSlice const body = this->parseVPackBody(success);
  if (!success) {
    // error already created
    return;
  }
  bool badFormat = !body.isArray();
  if (!badFormat) {
    for (VPackSlice entry : VPackArrayIterator(body)) {
      if (!entry.isString()) {
        badFormat = true;
        break;
      }
      RevisionId rev;
      if (encodeAsHLC) {
        rev = RevisionId::fromHLC(entry.stringView());
      } else {
        rev = RevisionId::fromSlice(entry);
      }
      if (rev == RevisionId::max()) {
        badFormat = true;
        break;
      }
    }
  }
  if (badFormat) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "body needs to be an array of revisions");
    return;
  }

  constexpr std::size_t sizeLimit = 16 * 1024 * 1024;
  std::size_t chunkSize = sizeLimit;

  bool found;
  std::string const& value = _request->value("chunkSize", found);

  if (found) {
    // query parameter "chunkSize" was specified
    chunkSize = StringUtils::uint64(value);
  }
  chunkSize = std::min(chunkSize, sizeLimit);

  std::size_t size = 0;  // running total, approximation
  RevisionReplicationIterator& it =
      *static_cast<RevisionReplicationIterator*>(ctx.iter.get());

  VPackBuffer<std::uint8_t> buffer;
  VPackBuilder response(buffer);
  {
    VPackArrayBuilder docs(&response);

    for (VPackSlice entry : VPackArrayIterator(body)) {
      RevisionId rev;
      if (encodeAsHLC) {
        rev = RevisionId::fromHLC(entry.stringView());
      } else {
        rev = RevisionId::fromSlice(entry);
      }
      // We assume that the rev is actually present, otherwise it would not
      // have been ordered. But we want this code to work if revisions in
      // the list arrive in some arbitrary order. However, in most cases
      // the list will contain adjacent revs in ascending order. Therefore,
      // we want to try a next() on the iterator first (if this makes sense).
      // However, if we then still have not found the right revision, we need
      // to seek. If the rev exists, the iterator will then point to the right
      // document:
      if (it.hasMore() && it.revision() < rev) {
        it.next();
      }
      if (!it.hasMore() || it.revision() != rev) {
        it.seek(rev);
      }
      VPackSlice res = it.hasMore() && it.revision() == rev
                           ? it.document()
                           : velocypack::Slice::emptyObjectSlice();

      auto byteSize = res.byteSize();
      if (size + byteSize > chunkSize && size > 0) {
        break;
      }
      response.add(res);
      size += byteSize;
    }
  }

  auto origin = transaction::OperationOriginInternal{
      "retrieving document revisions for replication"};
  auto ctxt = transaction::StandaloneContext::create(_vocbase, origin);
  generateResult(rest::ResponseCode::OK, std::move(buffer), ctxt);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determine the chunk size
////////////////////////////////////////////////////////////////////////////////

uint64_t RestReplicationHandler::determineChunkSize() const {
  // determine chunk size
  uint64_t chunkSize = defaultChunkSize;

  bool found;
  std::string const& value = _request->value("chunkSize", found);

  if (found) {
    // query parameter "chunkSize" was specified
    chunkSize = StringUtils::uint64(value);

    // don't allow overly big allocations
    if (chunkSize > maxChunkSize) {
      chunkSize = maxChunkSize;
    }
  }

  return chunkSize;
}

ReplicationApplier* RestReplicationHandler::getApplier(bool& global) {
  global = _request->parsedValue("global", false);

  if (global && _request->databaseName() != StaticStrings::SystemDatabase) {
    generateError(
        rest::ResponseCode::FORBIDDEN, TRI_ERROR_FORBIDDEN,
        "global inventory can only be created from within _system database");
    return nullptr;
  }

  if (global) {
    auto& replicationFeature =
        _vocbase.server().getFeature<ReplicationFeature>();
    return replicationFeature.globalReplicationApplier();
  } else {
    return _vocbase.replicationApplier();
  }
}

namespace {

struct RebootCookie final : public arangodb::TransactionState::Cookie {
  explicit RebootCookie(CallbackGuard&& g) noexcept : guard{std::move(g)} {}

  CallbackGuard guard;
};

}  // namespace

futures::Future<Result> RestReplicationHandler::createBlockingTransaction(
    TransactionId id, LogicalCollection& col, double ttl,
    AccessMode::Type access, RebootId const& rebootId,
    std::string const& serverId) {
  // it is ok to acquire the scope here in all cases. otherwise we
  // may block replication. note: we need this here in case the
  // leader is read-only (e.g. because the license has expired).
  // if we are not using superuser scope here and the leader is
  // read-only, trying to grab the lock in exclusive mode will
  // return a "read-only" error.
  ExecContextScope scope(ExecContext::superuserAsShared());

  transaction::Manager* mgr = transaction::ManagerFeature::manager();
  TRI_ASSERT(mgr != nullptr);

  std::vector<std::string> read;
  std::vector<std::string> exc;
  if (access == AccessMode::Type::READ) {
    read.push_back(col.name());
  } else {
    TRI_ASSERT(access == AccessMode::Type::EXCLUSIVE);
    exc.push_back(col.name());
  }
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  {
    auto const lockHeld = isLockHeld(id);
    TRI_ASSERT(lockHeld.is(TRI_ERROR_HTTP_NOT_FOUND))
        << "Unexpected error code " << lockHeld;
  }
#endif

  transaction::Options opts;
  opts.lockTimeout = ttl;  // not sure if appropriate ?
  Result res = co_await mgr->ensureManagedTrx(
      _vocbase, id, read, {}, exc, opts,
      transaction::OperationOriginInternal{
          "creating blocking transaction for follower catchup"},
      ttl);

  if (res.fail()) {
    co_return res;
  }

  std::string vn = _vocbase.name();

  // we successfully created the transaction.
  // make sure if it is aborted if something goes wrong.
  auto transactionAborter = scopeGuard([mgr, id, vn]() noexcept {
    try {
      mgr->abortManagedTrx(id, vn).waitAndGet();
    } catch (...) {
    }
  });

  try {
    if (!serverId.empty()) {
      std::string comment =
          absl::StrCat("SynchronizeShard from ", serverId, " for ", col.name(),
                       " access mode ", AccessMode::typeString(access));

      std::function<void(void)> f = [=]() {
        try {
          // Code does not matter, read only access, so we can roll back.
          transaction::Manager* mgr = transaction::ManagerFeature::manager();
          if (mgr) {
            mgr->abortManagedTrx(id, vn).waitAndGet();
          }
        } catch (...) {
          // All errors that show up here can only be
          // triggered if the query is destroyed in between.
        }
      };

      ClusterInfo& ci = server().getFeature<ClusterFeature>().clusterInfo();
      auto rGuard =
          std::make_unique<RebootCookie>(ci.rebootTracker().callMeOnChange(
              {serverId, rebootId}, std::move(f), std::move(comment)));
      auto ctx = co_await mgr->leaseManagedTrx(id, AccessMode::Type::WRITE,
                                               /*isSideUser*/ false);

      if (!ctx) {
        // Trx does not exist. So we assume it got cancelled.
        co_return {TRI_ERROR_TRANSACTION_INTERNAL,
                   "read transaction was cancelled"};
      }

      transaction::Methods trx{ctx};
      void* key = this;  // simon: not important to get it again
      trx.state()->cookie(key, std::move(rGuard));
    }

  } catch (...) {
    // For compatibility we only return this error
    co_return {TRI_ERROR_TRANSACTION_INTERNAL, "cannot begin read transaction"};
  }

  if (isTombstoned(id)) {
    // DO NOT LOCK in this case, pointless.
    // transactionAborter will abort the transaction for us.
    co_return {TRI_ERROR_TRANSACTION_INTERNAL, "transaction already cancelled"};
  }

  // when we get here, we let the transaction remain active until it
  // times out or a cancel request is sent.
  transactionAborter.cancel();

  co_return isLockHeld(id);
}

Result RestReplicationHandler::isLockHeld(TransactionId id) const {
  // The query is only hold for long during initial locking
  // there it should return false.
  // In all other cases it is released quickly.
  if (_vocbase.isDropped()) {
    return {TRI_ERROR_ARANGO_DATABASE_NOT_FOUND};
  }

  transaction::Manager* mgr = transaction::ManagerFeature::manager();
  TRI_ASSERT(mgr != nullptr);

  transaction::Status stats = mgr->getManagedTrxStatus(id);
  if (stats == transaction::Status::UNDEFINED) {
    return {TRI_ERROR_HTTP_NOT_FOUND,
            absl::StrCat("no hold read lock job found for id ", id.id())};
  }

  return {};
}

ResultT<bool> RestReplicationHandler::cancelBlockingTransaction(
    TransactionId id) const {
  // This lookup is only required for API compatibility,
  // otherwise an unconditional destroy() would do.
  auto res = isLockHeld(id);
  if (res.ok()) {
    transaction::Manager* mgr = transaction::ManagerFeature::manager();
    if (mgr) {
      auto isAborted = mgr->abortManagedTrx(id, _vocbase.name()).waitAndGet();
      if (isAborted.ok()) {  // lock was held
        return ResultT<bool>::success(true);
      }
      return ResultT<bool>::error(isAborted);
    }
  } else {
    registerTombstone(id);
  }
  return res;
}

futures::Future<ResultT<std::string>>
RestReplicationHandler::computeCollectionChecksum(
    TransactionId id, LogicalCollection* col) const {
  transaction::Manager* mgr = transaction::ManagerFeature::manager();
  if (!mgr) {
    co_return ResultT<std::string>::error(TRI_ERROR_SHUTTING_DOWN);
  }

  try {
    auto ctx = co_await mgr->leaseManagedTrx(id, AccessMode::Type::READ,
                                             /*isSideUser*/ false);
    if (!ctx) {
      // Trx does not exist. So we assume it got cancelled.
      co_return ResultT<std::string>::error(TRI_ERROR_TRANSACTION_INTERNAL,
                                            "read transaction was cancelled");
    }

    transaction::Methods trx(ctx);
    TRI_ASSERT(trx.status() == transaction::Status::RUNNING);

    uint64_t num = col->getPhysical()->numberDocuments(&trx);
    co_return ResultT<std::string>::success(std::to_string(num));
  } catch (...) {
    // Query exists, but is in use.
    // So in Locking phase
    co_return ResultT<std::string>::error(TRI_ERROR_TRANSACTION_INTERNAL,
                                          "Read lock not yet acquired!");
  }
}

static std::string IdToTombstoneKey(TRI_vocbase_t& vocbase, TransactionId id) {
  return vocbase.name() + "/" + StringUtils::itoa(id.id());
}

void RestReplicationHandler::timeoutTombstones() const {
  std::unordered_set<std::string> toDelete;
  {
    READ_LOCKER(readLocker, RestReplicationHandler::_tombLock);
    if (RestReplicationHandler::_tombstones.empty()) {
      // Fast path
      return;
    }
    auto now = std::chrono::steady_clock::now();
    for (auto const& it : RestReplicationHandler::_tombstones) {
      if (it.second < now) {
        toDelete.emplace(it.first);
      }
    }
    // Release read lock.
    // If someone writes now we do not really care.
  }
  if (toDelete.empty()) {
    // nothing todo
    return;
  }
  WRITE_LOCKER(writeLocker, RestReplicationHandler::_tombLock);
  for (auto const& it : toDelete) {
    try {
      RestReplicationHandler::_tombstones.erase(it);
    } catch (...) {
      // erase should not throw.
      TRI_ASSERT(false);
    }
  }
}

bool RestReplicationHandler::isTombstoned(TransactionId id) const {
  std::string key = IdToTombstoneKey(_vocbase, id);
  bool isDead = false;
  {
    READ_LOCKER(readLocker, RestReplicationHandler::_tombLock);
    isDead = RestReplicationHandler::_tombstones.find(key) !=
             RestReplicationHandler::_tombstones.end();
  }
  if (!isDead) {
    // Clear Tombstone
    WRITE_LOCKER(writeLocker, RestReplicationHandler::_tombLock);
    try {
      RestReplicationHandler::_tombstones.erase(key);
    } catch (...) {
      // Just ignore, tombstone will be removed by timeout, and IDs are unique
      // anyways
      TRI_ASSERT(false);
    }
  }
  return isDead;
}

void RestReplicationHandler::registerTombstone(TransactionId id) const {
  std::string key = IdToTombstoneKey(_vocbase, id);
  {
    WRITE_LOCKER(writeLocker, RestReplicationHandler::_tombLock);
    RestReplicationHandler::_tombstones.try_emplace(
        key, std::chrono::steady_clock::now() +
                 RestReplicationHandler::_tombstoneTimeout);
  }
  timeoutTombstones();
}
RequestLane RestReplicationHandler::lane() const {
  auto const& suffixes = _request->suffixes();

  size_t const len = suffixes.size();
  if (len >= 1) {
    std::string const& command = suffixes[0];
    if (command == AddFollower) {
      // Adding another server into the follower list
      // should be done quickly. The follower at this stage
      // is assumed to be in-sync, but cannot be used for failover
      // as long as it is not added. So get this sorted out quickly.
      return RequestLane::CLUSTER_INTERNAL;
    }
    if (command == HoldReadLockCollection) {
      if (_request->requestType() == RequestType::DELETE_REQ) {
        // A deleting request here, will allow as to unlock
        // the collection / shard in question.
        // In case of a hard-lock this shard is actually blocking
        // other operations. So let's hurry up with this.
        return RequestLane::CLUSTER_INTERNAL;
      }

      // This process will determine the start of a replication.
      // It can be delayed a bit and can be queued after other write
      // operations The follower is not in sync and requires to catch up
      // anyways.
      return RequestLane::SERVER_REPLICATION;
    }

    if (command == RemoveFollower || command == LoggerFollow ||
        command == Batch || command == Inventory || command == Revisions ||
        command == Dump) {
      return RequestLane::SERVER_REPLICATION_CATCHUP;
    }
  }
  return RequestLane::SERVER_REPLICATION;
}
