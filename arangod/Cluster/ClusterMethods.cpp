////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////


#include "ClusterMethods.h"

#include "Agency/TimeString.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/NumberUtils.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/system-functions.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterCollectionCreationInfo.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterTrxMethods.h"
#include "Cluster/ClusterTypes.h"
#include "Futures/Utilities.h"
#include "Graph/ClusterTraverserCache.h"
#include "Graph/Traverser.h"
#include "Graph/TraverserOptions.h"
#include "Network/ClusterUtils.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"
#include "Rest/Version.h"
#include "Sharding/ShardingInfo.h"
#include "StorageEngine/HotBackupCommon.h"
#include "StorageEngine/TransactionCollection.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Context.h"
#include "Transaction/Helpers.h"
#include "Transaction/Manager.h"
#include "Transaction/ManagerFeature.h"
#include "Transaction/Methods.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/OperationOptions.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/Methods/Version.h"
#include "VocBase/ticks.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/RocksDBEngine/RocksDBHotBackup.h"
#endif
#include "StorageEngine/EngineSelectorFeature.h"
#include "ClusterEngine/Common.h"
#include "ClusterEngine/ClusterEngine.h"

#include <velocypack/Buffer.h>
#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include "velocypack/StringRef.h"
#include <velocypack/velocypack-aliases.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <numeric>
#include <vector>
#include <random>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::futures;
using namespace arangodb::rest;

using Helper = arangodb::basics::VelocyPackHelper;

// Timeout for write operations, note that these are used for communication
// with a shard leader and we always have to assume that some follower has
// stopped writes for some time to get in sync:
static double const CL_DEFAULT_LONG_TIMEOUT = 900.0;

namespace {
template <typename T>
T addFigures(VPackSlice const& v1, VPackSlice const& v2,
             std::vector<std::string> const& attr) {
  TRI_ASSERT(v1.isObject());
  TRI_ASSERT(v2.isObject());

  T value = 0;

  VPackSlice found = v1.get(attr);
  if (found.isNumber()) {
    value += found.getNumericValue<T>();
  }

  found = v2.get(attr);
  if (found.isNumber()) {
    value += found.getNumericValue<T>();
  }

  return value;
}

/// @brief begin a transaction on some leader shards
template <typename ShardDocsMap>
Future<Result> beginTransactionOnSomeLeaders(TransactionState& state,
                                             LogicalCollection const& coll,
                                             ShardDocsMap const& shards) {
  TRI_ASSERT(state.isCoordinator());
  TRI_ASSERT(!state.hasHint(transaction::Hints::Hint::SINGLE_OPERATION));

  std::shared_ptr<ShardMap> shardMap = coll.shardIds();
  std::vector<std::string> leaders;

  for (auto const& pair : shards) {
    auto const& it = shardMap->find(pair.first);
    if (it->second.empty()) {
      return TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE;  // something is broken
    }
    // now we got the shard leader
    std::string const& leader = it->second[0];
    if (!state.knowsServer(leader)) {
      leaders.emplace_back(leader);
    }
  }
  return ClusterTrxMethods::beginTransactionOnLeaders(state, leaders);
}

// begin transaction on shard leaders
Future<Result> beginTransactionOnAllLeaders(transaction::Methods& trx, ShardMap const& shards) {
  TRI_ASSERT(trx.state()->isCoordinator());
  TRI_ASSERT(trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED));
  std::vector<ServerID> leaders;
  for (auto const& shardServers : shards) {
    ServerID const& srv = shardServers.second.at(0);
    if (!trx.state()->knowsServer(srv)) {
      leaders.emplace_back(srv);
    }
  }
  return ClusterTrxMethods::beginTransactionOnLeaders(*trx.state(), leaders);
}

/// @brief add the correct header for the shard
void addTransactionHeaderForShard(transaction::Methods const& trx, ShardMap const& shardMap,
                                  ShardID const& shard, network::Headers& headers) {
  TRI_ASSERT(trx.state()->isCoordinator());
  auto const& it = shardMap.find(shard);
  if (it != shardMap.end()) {
    if (it->second.empty()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE);
    }
    ServerID const& leader = it->second[0];
    ClusterTrxMethods::addTransactionHeader(trx, leader, headers);
  } else {
    TRI_ASSERT(false);
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "couldnt find shard in shardMap");
  }
}

/// @brief iterate over shard responses and compile a result
/// This will take care of checking the fuerte responses. If the response has
/// a body, then the callback will be called on the body, with access to the
/// result-so-far. In particular, a VPackBuilder is initialized to empty before
/// handling any response. It will be passed to the pre callback (default noop)
/// for initialization, then it will be passed to each instantiation of the
/// handler callback, reduce-style. Finally, it will be passed to the post
/// callback and then returned via the OperationResult.
OperationResult handleResponsesFromAllShards(
    OperationOptions const& options,
    std::vector<futures::Try<arangodb::network::Response>>& responses,
    std::function<void(Result&, VPackBuilder&, ShardID&, VPackSlice)> handler,
    std::function<void(Result&, VPackBuilder&)> pre = [](Result&, VPackBuilder&) -> void {},
    std::function<void(Result&, VPackBuilder&)> post = [](Result&, VPackBuilder&) -> void {}) {
  // If none of the shards responds we return a SERVER_ERROR;
  Result result;
  VPackBuilder builder;
  pre(result, builder);
  if (result.fail()) {
    return OperationResult(result, builder.steal(), options);
  }
  for (Try<arangodb::network::Response> const& tryRes : responses) {
    network::Response const& res = tryRes.get();  // throws exceptions upwards
    ShardID sId = res.destinationShard();
    int commError = network::fuerteToArangoErrorCode(res);
    if (commError != TRI_ERROR_NO_ERROR) {
      result.reset(commError);
      break;
    } else {
      std::vector<VPackSlice> const& slices = res.response->slices();
      if (!slices.empty()) {
        VPackSlice answer = slices[0];
        handler(result, builder, sId, answer);
        if (result.fail()) {
          break;
        }
      }
    }
  }
  post(result, builder);
  return OperationResult(result, builder.steal(), options);
}

// velocypack representation of object
// {"error":true,"errorMessage":"document not found","errorNum":1202}
const char* notFoundSlice =
  "\x14\x36\x45\x65\x72\x72\x6f\x72\x1a\x4c\x65\x72\x72\x6f\x72\x4d"
  "\x65\x73\x73\x61\x67\x65\x52\x64\x6f\x63\x75\x6d\x65\x6e\x74\x20"
  "\x6e\x6f\x74\x20\x66\x6f\x75\x6e\x64\x48\x65\x72\x72\x6f\x72\x4e"
  "\x75\x6d\x29\xb2\x04\x03";

////////////////////////////////////////////////////////////////////////////////
/// @brief merge the baby-object results. (all shards version)
///        results contians the result from all shards in any order.
///        resultBody will be cleared and contains the merged result after this
///        function
///        errorCounter will correctly compute the NOT_FOUND counter, all other
///        codes remain unmodified.
///
///        The merge is executed the following way:
///        FOR every expected document we scan iterate over the corresponding
///        response
///        of each shard. If any of them returned sth. different than NOT_FOUND
///        we take this result as correct.
///        If none returned sth different than NOT_FOUND we return NOT_FOUND as
///        well
////////////////////////////////////////////////////////////////////////////////
void mergeResultsAllShards(std::vector<VPackSlice> const& results, VPackBuilder& resultBody,
                           std::unordered_map<int, size_t>& errorCounter,
                           VPackValueLength const expectedResults) {
  // errorCounter is not allowed to contain any NOT_FOUND entry.
  TRI_ASSERT(errorCounter.find(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) ==
             errorCounter.end());
  size_t realNotFound = 0;

  resultBody.clear();
  resultBody.openArray();
  for (VPackValueLength currentIndex = 0; currentIndex < expectedResults; ++currentIndex) {
    bool foundRes = false;
    for (VPackSlice oneRes : results) {
      TRI_ASSERT(oneRes.isArray());
      oneRes = oneRes.at(currentIndex);

      int errorNum = TRI_ERROR_NO_ERROR;
      VPackSlice errorNumSlice = oneRes.get(StaticStrings::ErrorNum);
      if (errorNumSlice.isNumber()) {
        errorNum = errorNumSlice.getNumber<int>();
      }
      if ((errorNum != TRI_ERROR_NO_ERROR && errorNum != TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) ||
          oneRes.hasKey(StaticStrings::KeyString)) {
        // This is the correct result: Use it
        resultBody.add(oneRes);
        foundRes = true;
        break;
      }
    }
    if (!foundRes) {
      // Found none, use static NOT_FOUND
      resultBody.add(VPackSlice(reinterpret_cast<uint8_t const*>(::notFoundSlice)));
      realNotFound++;
    }
  }
  resultBody.close();
  if (realNotFound > 0) {
    errorCounter.try_emplace(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND, realNotFound);
  }
}

/// @brief handle CRUD api shard responses, slow path
template <typename F, typename CT>
OperationResult handleCRUDShardResponsesFast(F&& func, CT const& opCtx,
                                             std::vector<Try<network::Response>> const& results) {
  std::map<ShardID, VPackSlice> resultMap;
  std::map<ShardID, int> shardError;
  std::unordered_map<int, size_t> errorCounter;

  fuerte::StatusCode code = fuerte::StatusInternalError;
  // If none of the shards responded we return a SERVER_ERROR;

  for (Try<arangodb::network::Response> const& tryRes : results) {
    network::Response const& res = tryRes.get();  // throws exceptions upwards
    ShardID sId = res.destinationShard();

    int commError = network::fuerteToArangoErrorCode(res);
    if (commError != TRI_ERROR_NO_ERROR) {
      shardError.try_emplace(sId, commError);
    } else {
      resultMap.try_emplace(sId, res.response->slice());
      network::errorCodesFromHeaders(res.response->header.meta(), errorCounter, true);
      code = res.response->statusCode();
    }
  }

  // merge the baby-object results. reverseMapping contains
  // the ordering of elements, the vector in this
  // map is expected to be sorted from front to back.
  // resultMap contains the answers for each shard.
  // It is guaranteed that the resulting array indexes are
  // equal to the original request ordering before it was destructured

  VPackBuilder resultBody;
  resultBody.openArray();
  for (auto const& pair : opCtx.reverseMapping) {
    ShardID const& sId = pair.first;
    auto const& it = resultMap.find(sId);
    if (it == resultMap.end()) { // no answer from this shard
      auto const& it2 = shardError.find(sId);
      TRI_ASSERT(it2 != shardError.end());

      auto weSend = opCtx.shardMap.find(sId);
      TRI_ASSERT(weSend != opCtx.shardMap.end());  // We send sth there earlier.
      size_t count = weSend->second.size();
      for (size_t i = 0; i < count; ++i) {
        resultBody.openObject(/*unindexed*/ true);
        resultBody.add(StaticStrings::Error, VPackValue(true));
        resultBody.add(StaticStrings::ErrorNum, VPackValue(it2->second));
        resultBody.close();
      }

    } else {
      VPackSlice arr = it->second;
      // we expect an array of baby-documents, but the response might
      // also be an error, if the DBServer threw a hissy fit
      if (arr.isObject() && arr.hasKey(StaticStrings::Error) &&
          arr.get(StaticStrings::Error).isBoolean() &&
          arr.get(StaticStrings::Error).getBoolean()) {
        // an error occurred, now rethrow the error
        int res = arr.get(StaticStrings::ErrorNum).getNumericValue<int>();
        VPackSlice msg = arr.get(StaticStrings::ErrorMessage);
        if (msg.isString()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(res, msg.copyString());
        } else {
          THROW_ARANGO_EXCEPTION(res);
        }
      }
      resultBody.add(arr.at(pair.second));
    }
  }
  resultBody.close();

  return std::forward<F>(func)(code, resultBody.steal(),
                               std::move(opCtx.options), std::move(errorCounter));
}

/// @brief handle CRUD api shard responses, slow path
template <typename F>
OperationResult handleCRUDShardResponsesSlow(F&& func, size_t expectedLen, OperationOptions options,
                                             std::vector<Try<network::Response>> const& responses) {
  if (expectedLen == 0) {  // Only one can answer, we react a bit differently
    std::shared_ptr<VPackBuffer<uint8_t>> buffer;

    int nrok = 0;
    int commError = TRI_ERROR_NO_ERROR;
    fuerte::StatusCode code;
    for (size_t i = 0; i < responses.size(); i++) {
      network::Response const& res = responses[i].get();

      if (res.error == fuerte::Error::NoError) {
        // if no shard has the document, use NF answer from last shard
        const bool isNotFound = res.response->statusCode() == fuerte::StatusNotFound;
        if (!isNotFound || (isNotFound && nrok == 0 && i == responses.size() - 1)) {
          nrok++;
          code = res.response->statusCode();
          buffer = res.response->stealPayload();
        }
      } else {
        commError = network::fuerteToArangoErrorCode(res);
      }
    }

    if (nrok == 0) {  // This can only happen, if a commError was encountered!
      return OperationResult(commError, options);
    }
    if (nrok > 1) {
      return OperationResult(TRI_ERROR_CLUSTER_GOT_CONTRADICTING_ANSWERS, options);
    }

    return std::forward<F>(func)(code, std::move(buffer), std::move(options), {});
  }

  // We select all results from all shards and merge them back again.
  std::vector<VPackSlice> allResults;
  allResults.reserve(responses.size());

  std::unordered_map<int, size_t> errorCounter;
  // If no server responds we return 500
  for (Try<network::Response> const& tryRes : responses) {
    network::Response const& res = tryRes.get();
    if (res.error != fuerte::Error::NoError) {
      return OperationResult(network::fuerteToArangoErrorCode(res), options);
    }

    allResults.push_back(res.response->slice());
    network::errorCodesFromHeaders(res.response->header.meta(), errorCounter,
                                   /*includeNotFound*/ false);
  }
  VPackBuilder resultBody;
  // If we get here we get exactly one result for every shard.
  TRI_ASSERT(allResults.size() == responses.size());
  mergeResultsAllShards(allResults, resultBody, errorCounter, expectedLen);
  return OperationResult(Result(), resultBody.steal(), std::move(options),
                         std::move(errorCounter));
}

/// @brief class to move values across future handlers
struct CrudOperationCtx {
  std::vector<std::pair<ShardID, VPackValueLength>> reverseMapping;
  std::map<ShardID, std::vector<VPackSlice>> shardMap;
  arangodb::OperationOptions options;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Distribute one document onto a shard map. If this returns
///        TRI_ERROR_NO_ERROR the correct shard could be determined, if
///        it returns sth. else this document is NOT contained in the shardMap
////////////////////////////////////////////////////////////////////////////////

int distributeBabyOnShards(CrudOperationCtx& opCtx,
                           LogicalCollection& collinfo,
                           VPackSlice const value) {
  TRI_ASSERT(!collinfo.isSmart() || collinfo.type() == TRI_COL_TYPE_DOCUMENT);

  ShardID shardID;
  if (!value.isString() && !value.isObject()) {
    // We have invalid input at this point.
    // However we can work with the other babies.
    // This is for compatibility with single server
    // We just assign it to any shard and pretend the user has given a key
    shardID = collinfo.shardingInfo()->shardListAsShardID()->at(0);
  } else {
    // Now find the responsible shard:
    bool usesDefaultShardingAttributes;
    int res = collinfo.getResponsibleShard(value, /*docComplete*/false, shardID,
                                           usesDefaultShardingAttributes);

    if (res == TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) {
      return TRI_ERROR_CLUSTER_SHARD_GONE;
    }
    if (res != TRI_ERROR_NO_ERROR) {
      // We can not find a responsible shard
      return res;
    }
  }

  // We found the responsible shard. Add it to the list.
  auto it = opCtx.shardMap.find(shardID);
  if (it == opCtx.shardMap.end()) {
    opCtx.shardMap.try_emplace(shardID, std::vector<VPackSlice>{value});
    opCtx.reverseMapping.emplace_back(shardID, 0);
  } else {
    it->second.push_back(value);
    opCtx.reverseMapping.emplace_back(shardID, it->second.size() - 1);
  }
  return TRI_ERROR_NO_ERROR;
}

struct CreateOperationCtx {
  std::vector<std::pair<ShardID, VPackValueLength>> reverseMapping;
  std::map<ShardID, std::vector<std::pair<VPackSlice, std::string>>> shardMap;
  arangodb::OperationOptions options;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief Distribute one document onto a shard map. If this returns
///        TRI_ERROR_NO_ERROR the correct shard could be determined, if
///        it returns sth. else this document is NOT contained in the shardMap.
///        Also generates a key if necessary.
////////////////////////////////////////////////////////////////////////////////

int distributeBabyOnShards(CreateOperationCtx& opCtx,
                           LogicalCollection& collinfo,
                           VPackSlice const value, bool isRestore) {
  ShardID shardID;
  std::string _key;

  if (!value.isObject()) {
    // We have invalid input at this point.
    // However we can work with the other babies.
    // This is for compatibility with single server
    // We just assign it to any shard and pretend the user has given a key
    shardID = collinfo.shardingInfo()->shardListAsShardID()->at(0);
  } else {
    int r = transaction::Methods::validateSmartJoinAttribute(collinfo, value);

    if (r != TRI_ERROR_NO_ERROR) {
      return r;
    }

    // Sort out the _key attribute:
    // The user is allowed to specify _key, provided that _key is the one
    // and only sharding attribute, because in this case we can delegate
    // the responsibility to make _key attributes unique to the responsible
    // shard. Otherwise, we ensure uniqueness here and now by taking a
    // cluster-wide unique number. Note that we only know the sharding
    // attributes a bit further down the line when we have determined
    // the responsible shard.

    bool userSpecifiedKey = false;
    VPackSlice keySlice = value.get(StaticStrings::KeyString);
    if (keySlice.isNone()) {
      // The user did not specify a key, let's create one:
      _key = collinfo.keyGenerator()->generate();
    } else {
      userSpecifiedKey = true;
      if (keySlice.isString()) {
        VPackValueLength l;
        char const* p = keySlice.getString(l);
        collinfo.keyGenerator()->track(p, l);
      }
    }

    // Now find the responsible shard:
    bool usesDefaultShardingAttributes;
    int error = TRI_ERROR_NO_ERROR;
    if (userSpecifiedKey) {
      error = collinfo.getResponsibleShard(value, /*docComplete*/true, shardID,
                                           usesDefaultShardingAttributes);
    } else {
      // we pass in the generated _key so we do not need to rebuild the input slice
      error = collinfo.getResponsibleShard(value, /*docComplete*/true, shardID,
                                           usesDefaultShardingAttributes, VPackStringRef(_key));
    }
    if (error == TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND) {
      return TRI_ERROR_CLUSTER_SHARD_GONE;
    }

    // Now perform the above mentioned check:
    if (userSpecifiedKey &&
        (!usesDefaultShardingAttributes || !collinfo.allowUserKeys()) && !isRestore) {
      return TRI_ERROR_CLUSTER_MUST_NOT_SPECIFY_KEY;
    }
  }

  // We found the responsible shard. Add it to the list.
  auto it = opCtx.shardMap.find(shardID);
  if (it == opCtx.shardMap.end()) {
    opCtx.shardMap.try_emplace(shardID, std::vector<std::pair<VPackSlice, std::string>>{{value, _key}});
    opCtx.reverseMapping.emplace_back(shardID, 0);
  } else {
    it->second.emplace_back(value, _key);
    opCtx.reverseMapping.emplace_back(shardID, it->second.size() - 1);
  }
  return TRI_ERROR_NO_ERROR;
}
}  // namespace

////////////////////////////////////////////////////////////////////////////////
/// @brief compute a shard distribution for a new collection, the list
/// dbServers must be a list of DBserver ids to distribute across.
/// If this list is empty, the complete current list of DBservers is
/// fetched from ClusterInfo and with shuffle to mix it up.
////////////////////////////////////////////////////////////////////////////////

static std::shared_ptr<std::unordered_map<std::string, std::vector<std::string>>> DistributeShardsEvenly(
    ClusterInfo& ci, uint64_t numberOfShards, uint64_t replicationFactor,
    std::vector<std::string>& dbServers, bool warnAboutReplicationFactor) {
  auto shards =
      std::make_shared<std::unordered_map<std::string, std::vector<std::string>>>();

  if (dbServers.empty()) {
    ci.loadCurrentDBServers();
    dbServers = ci.getCurrentDBServers();
    if (dbServers.empty()) {
      return shards;
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(dbServers.begin(), dbServers.end(), g);
  }

  // mop: distribute SatelliteCollections on all servers
  if (replicationFactor == 0) {
    replicationFactor = dbServers.size();
  }

  // fetch a unique id for each shard to create
  uint64_t const id = ci.uniqid(numberOfShards);

  size_t leaderIndex = 0;
  size_t followerIndex = 0;
  for (uint64_t i = 0; i < numberOfShards; ++i) {
    // determine responsible server(s)
    std::vector<std::string> serverIds;
    for (uint64_t j = 0; j < replicationFactor; ++j) {
      if (j >= dbServers.size()) {
        if (warnAboutReplicationFactor) {
          LOG_TOPIC("e16ec", WARN, Logger::CLUSTER)
              << "createCollectionCoordinator: replicationFactor is "
              << "too large for the number of DBservers";
        }
        break;
      }
      std::string candidate;
      // mop: leader
      if (serverIds.empty()) {
        candidate = dbServers[leaderIndex++];
        if (leaderIndex >= dbServers.size()) {
          leaderIndex = 0;
        }
      } else {
        do {
          candidate = dbServers[followerIndex++];
          if (followerIndex >= dbServers.size()) {
            followerIndex = 0;
          }
        } while (candidate == serverIds[0]);  // mop: ignore leader
      }
      serverIds.push_back(candidate);
    }

    // determine shard id
    std::string shardId = "s" + StringUtils::itoa(id + i);

    shards->try_emplace(shardId, serverIds);
  }

  return shards;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Clone shard distribution from other collection
////////////////////////////////////////////////////////////////////////////////

static std::shared_ptr<std::unordered_map<std::string, std::vector<std::string>>> CloneShardDistribution(
    ClusterInfo& ci, std::shared_ptr<LogicalCollection> col,
    std::shared_ptr<LogicalCollection> const& other) {
  TRI_ASSERT(col);
  TRI_ASSERT(other);

  if (!other->distributeShardsLike().empty()) {
    CollectionNameResolver resolver(col->vocbase());
    std::string name = other->distributeShardsLike();
    DataSourceId cid{arangodb::basics::StringUtils::uint64(name)};
    if (cid.isSet()) {
      name = resolver.getCollectionNameCluster(cid);
    }
    std::string const errorMessage = "Cannot distribute shards like '" + other->name() +
                                     "' it is already distributed like '" + name + "'.";
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_CHAIN_OF_DISTRIBUTESHARDSLIKE, errorMessage);
  }

  auto result =
      std::make_shared<std::unordered_map<std::string, std::vector<std::string>>>();

  // We need to replace the distribute with the cid.
  auto cidString = arangodb::basics::StringUtils::itoa(other.get()->id().id());
  col->distributeShardsLike(cidString, other->shardingInfo());

  if (col->isSmart() && col->type() == TRI_COL_TYPE_EDGE) {
    return result;
  }

  auto numberOfShards = static_cast<uint64_t>(col->numberOfShards());

  // Here we need to make sure that we put the shards and
  // shard distribution in the correct order: shards need
  // to be sorted alphabetically by ID
  //
  // shardIds() returns an unordered_map<ShardID, std::vector<ServerID>>
  // so the method is a bit mis-named.
  auto otherShardsMap = other->shardIds();

  // TODO: This should really be a utility function, possibly in ShardingInfo?
  std::vector<ShardID> otherShards;
  for (auto& it : *otherShardsMap) {
    otherShards.push_back(it.first);
  }
  ShardingInfo::sortShardNamesNumerically(otherShards);

  TRI_ASSERT(numberOfShards == otherShards.size());

  // fetch a unique id for each shard to create
  uint64_t const id = ci.uniqid(numberOfShards);
  for (uint64_t i = 0; i < numberOfShards; ++i) {
    // determine responsible server(s)
    std::string shardId = "s" + StringUtils::itoa(id + i);
    result->try_emplace(std::move(shardId), otherShardsMap->at(otherShards.at(i)));
  }
  return result;
}

namespace arangodb {

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a copy of all HTTP headers to forward
////////////////////////////////////////////////////////////////////////////////

network::Headers getForwardableRequestHeaders(arangodb::GeneralRequest* request) {
  network::Headers result;

  for (auto const& it : request->headers()) {
    std::string const& key = it.first;

    // ignore the following headers
    if (key != "x-arango-async" && key != "authorization" &&
        key != "content-length" && key != "connection" && key != "expect" &&
        key != "host" && key != "origin" && key != StaticStrings::HLCHeader &&
        key != StaticStrings::ErrorCodes &&
        key.compare(0, 14, "access-control", 14) != 0) {
      result.try_emplace(key, it.second);
    }
  }

  result["content-length"] = StringUtils::itoa(request->contentLength());

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check if a list of attributes have the same values in two vpack
/// documents
////////////////////////////////////////////////////////////////////////////////

bool shardKeysChanged(LogicalCollection const& collection, VPackSlice const& oldValue,
                      VPackSlice const& newValue, bool isPatch) {
  if (!oldValue.isObject() || !newValue.isObject()) {
    // expecting two objects. everything else is an error
    return true;
  }
#ifdef DEBUG_SYNC_REPLICATION
  if (collection.vocbase().name() == "sync-replication-test") {
    return false;
  }
#endif

  std::vector<std::string> const& shardKeys = collection.shardKeys();

  for (const auto & shardKey : shardKeys) {
    if (shardKey == StaticStrings::KeyString) {
      continue;
    }

    VPackSlice n = newValue.get(shardKey);

    if (n.isNone() && isPatch) {
      // attribute not set in patch document. this means no update
      continue;
    }

    VPackSlice o = oldValue.get(shardKey);

    if (o.isNone()) {
      // if attribute is undefined, use "null" instead
      o = arangodb::velocypack::Slice::nullSlice();
    }

    if (n.isNone()) {
      // if attribute is undefined, use "null" instead
      n = arangodb::velocypack::Slice::nullSlice();
    }

    if (!Helper::equal(n, o, false)) {
      return true;
    }
  }

  return false;
}

bool smartJoinAttributeChanged(LogicalCollection const& collection, VPackSlice const& oldValue,
                               VPackSlice const& newValue, bool isPatch) {
  if (!collection.hasSmartJoinAttribute()) {
    return false;
  }
  if (!oldValue.isObject() || !newValue.isObject()) {
    // expecting two objects. everything else is an error
    return true;
  }

  std::string const& s = collection.smartJoinAttribute();

  VPackSlice n = newValue.get(s);
  if (!n.isString()) {
    if (isPatch && n.isNone()) {
      // attribute not set in patch document. this means no update
      return false;
    }

    // no string value... invalid!
    return true;
  }

  VPackSlice o = oldValue.get(s);
  TRI_ASSERT(o.isString());

  return !Helper::equal(n, o, false);
}

void aggregateClusterFigures(bool details, bool isSmartEdgeCollectionPart, 
                             VPackSlice value, VPackBuilder& builder) {
  TRI_ASSERT(value.isObject());
  TRI_ASSERT(builder.slice().isObject());
  TRI_ASSERT(builder.isClosed());

  VPackBuilder updated;

  updated.openObject();

  bool cacheInUse = Helper::getBooleanValue(value, "cacheInUse", false);
  bool totalCacheInUse = cacheInUse || Helper::getBooleanValue(builder.slice(), "cacheInUse", false);
  updated.add("cacheInUse", VPackValue(totalCacheInUse));

  if (cacheInUse) {
    updated.add("cacheLifeTimeHitRate", VPackValue(addFigures<double>(value, builder.slice(),
                                                                      {"cacheLifeTimeHitRate"})));
    updated.add("cacheWindowedHitRate", VPackValue(addFigures<double>(value, builder.slice(),
                                                                      {"cacheWindowedHitRate"})));
  }
  updated.add("cacheSize", VPackValue(addFigures<size_t>(value, builder.slice(),
                                                               {"cacheSize"})));
  updated.add("cacheUsage", VPackValue(addFigures<size_t>(value, builder.slice(),
                                                               {"cacheUsage"})));
  updated.add("documentsSize", VPackValue(addFigures<size_t>(value, builder.slice(),
                                                               {"documentsSize"})));

  updated.add("indexes", VPackValue(VPackValueType::Object));
  VPackSlice indexes = builder.slice().get("indexes");
  if (isSmartEdgeCollectionPart || indexes.isObject()) {
    // don't count indexes multiple times - all shards have the same indexes!
    // in addition: don't count the indexes from the sub-collections of a smart
    // edge collection multiple times!
    updated.add("count", indexes.get("count"));
  } else {
    updated.add("count", VPackValue(addFigures<size_t>(value, builder.slice(),
                                                       {"indexes", "count"})));
  }
  updated.add("size", VPackValue(addFigures<size_t>(value, builder.slice(),
                                                    {"indexes", "size"})));
  updated.close(); // "indexes"

  if (details && value.hasKey("engine")) {
    updated.add("engine", VPackValue(VPackValueType::Object));
    if (isSmartEdgeCollectionPart) {
      // don't count documents from the sub-collections of a smart edge collection
      // multiple times
      updated.add("documents", builder.slice().get(std::vector<std::string>{"engine", "documents"}));
    } else {
      updated.add("documents", VPackValue(addFigures<size_t>(value, builder.slice(),
                                                             {"engine", "documents"})));
    }
    // merge indexes together
    std::map<uint64_t, std::pair<VPackSlice, VPackSlice>> indexes;

    updated.add("indexes", VPackValue(VPackValueType::Array));
    VPackSlice rocksDBValues = value.get("engine");

    if (!isSmartEdgeCollectionPart) {
      for (auto const& it : VPackArrayIterator(rocksDBValues.get("indexes"))) {
        VPackSlice idSlice = it.get("id");
        if (!idSlice.isNumber()) {
          continue;
        }
        indexes.emplace(idSlice.getNumber<uint64_t>(), std::make_pair(it, VPackSlice::noneSlice()));
      }
    }
  
    rocksDBValues = builder.slice().get("engine");
    if (rocksDBValues.isObject()) {
      for (auto const& it : VPackArrayIterator(rocksDBValues.get("indexes"))) {
        VPackSlice idSlice = it.get("id");
        if (!idSlice.isNumber()) {
          continue;
        }
        auto idx = indexes.find(idSlice.getNumber<uint64_t>());
        if (idx == indexes.end()) {
          indexes.emplace(idSlice.getNumber<uint64_t>(), std::make_pair(it, VPackSlice::noneSlice()));
        } else {
          (*idx).second.second = it;
        }
      }
    }

    for (auto const& it : indexes) {
      updated.openObject();
      updated.add("type", it.second.first.get("type"));
      updated.add("id", it.second.first.get("id"));
      uint64_t count = it.second.first.get("count").getNumber<uint64_t>();
      if (it.second.second.isObject()) {
        count += it.second.second.get("count").getNumber<uint64_t>();
      }
      updated.add("count", VPackValue(count));
      updated.close();
    }

    updated.close(); // "indexes" array
    updated.close(); // "engine" object
  }

  updated.close();

  TRI_ASSERT(updated.slice().isObject());
  TRI_ASSERT(updated.isClosed());

  builder = VPackCollection::merge(builder.slice(), updated.slice(), true, false);
  TRI_ASSERT(builder.slice().isObject());
  TRI_ASSERT(builder.isClosed());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns revision for a sharded collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> revisionOnCoordinator(ClusterFeature& feature,
                                                       std::string const& dbname,
                                                       std::string const& collname,
                                                       OperationOptions const& options) {
  // Set a few variables needed for our work:
  ClusterInfo& ci = feature.clusterInfo();

  // First determine the collection ID from the name:
  std::shared_ptr<LogicalCollection> collinfo;
  collinfo = ci.getCollectionNT(dbname, collname);
  if (collinfo == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, options));
  }

  network::RequestOptions reqOpts;
  reqOpts.database = dbname;
  reqOpts.timeout = network::Timeout(300.0);

  // If we get here, the sharding attributes are not only _key, therefore
  // we have to contact everybody:
  std::shared_ptr<ShardMap> shards = collinfo->shardIds();
  std::vector<Future<network::Response>> futures;
  futures.reserve(shards->size());

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();
  for (auto const& p : *shards) {
    auto future =
        network::sendRequest(pool, "shard:" + p.first, fuerte::RestVerb::Get,
                             "/_api/collection/" + StringUtils::urlEncode(p.first) + "/revision",
                             VPackBuffer<uint8_t>(), reqOpts);
    futures.emplace_back(std::move(future));
  }

  auto cb = [options](std::vector<Try<network::Response>>&& results) -> OperationResult {
    return handleResponsesFromAllShards(
        options, results,
        [](Result& result, VPackBuilder& builder, ShardID&, VPackSlice answer) -> void {
          if (answer.isObject()) {
            VPackSlice r = answer.get("revision");
            if (r.isString()) {
              VPackValueLength len;
              char const* p = r.getString(len);
              RevisionId cmp = RevisionId::fromString(p, len, false);
              RevisionId rid = RevisionId::fromSlice(builder.slice());
              if (cmp != RevisionId::max() && cmp > rid) {
                // get the maximum value
                builder.clear();
                builder.add(VPackValue(cmp.id()));
              }
            }
          } else {
            // didn't get the expected response
            result.reset(TRI_ERROR_INTERNAL);
          }
        });
  };
  return futures::collectAll(std::move(futures)).thenValue(std::move(cb));
}

futures::Future<Result> warmupOnCoordinator(ClusterFeature& feature,
                                            std::string const& dbname,
                                            std::string const& cid,
                                            OperationOptions const& options) {
  // Set a few variables needed for our work:
  ClusterInfo& ci = feature.clusterInfo();

  // First determine the collection ID from the name:
  std::shared_ptr<LogicalCollection> collinfo;
  collinfo = ci.getCollectionNT(dbname, cid);
  if (collinfo == nullptr) {
    return futures::makeFuture(Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND));
  }

  // If we get here, the sharding attributes are not only _key, therefore
  // we have to contact everybody:
  std::shared_ptr<ShardMap> shards = collinfo->shardIds();

  network::RequestOptions opts;
  opts.database = dbname;
  opts.timeout = network::Timeout(300.0);

  std::vector<Future<network::Response>> futures;
  futures.reserve(shards->size());

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();
  for (auto const& p : *shards) {
    // handler expects valid velocypack body (empty object minimum)
    VPackBuffer<uint8_t> buffer;
    buffer.append(VPackSlice::emptyObjectSlice().begin(), 1);

    auto future =
        network::sendRequest(pool, "shard:" + p.first, fuerte::RestVerb::Get,
                             "/_api/collection/" + StringUtils::urlEncode(p.first) +
                                 "/loadIndexesIntoMemory",
                             std::move(buffer), opts);
    futures.emplace_back(std::move(future));
  }

  auto cb = [options](std::vector<Try<network::Response>>&& results) -> OperationResult {
    return handleResponsesFromAllShards(options, results,
                                        [](Result&, VPackBuilder&, ShardID&, VPackSlice) -> void {
                                          // we don't care about response bodies, just that the requests succeeded
                                        });
  };
  return futures::collectAll(std::move(futures))
      .thenValue(std::move(cb))
      .thenValue([](OperationResult&& opRes) -> Result { return opRes.result; });
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns figures for a sharded collection
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> figuresOnCoordinator(ClusterFeature& feature,
                                                      std::string const& dbname,
                                                      std::string const& collname, bool details,
                                                      OperationOptions const& options) {
  // Set a few variables needed for our work:
  ClusterInfo& ci = feature.clusterInfo();

  // First determine the collection ID from the name:
  std::shared_ptr<LogicalCollection> collinfo;
  collinfo = ci.getCollectionNT(dbname, collname);
  if (collinfo == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, options));
  }

  network::RequestOptions reqOpts;
  reqOpts.database = dbname;
  reqOpts.param("details", details ? "true" : "false");
  reqOpts.timeout = network::Timeout(300.0);

  // If we get here, the sharding attributes are not only _key, therefore
  // we have to contact everybody:
  std::shared_ptr<ShardMap> shards = collinfo->shardIds();
  std::vector<Future<network::Response>> futures;
  futures.reserve(shards->size());

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();
  for (auto const& p : *shards) {
    auto future =
        network::sendRequest(pool, "shard:" + p.first, fuerte::RestVerb::Get,
                             "/_api/collection/" +
                                 StringUtils::urlEncode(p.first) + "/figures",
                             VPackBuffer<uint8_t>(), reqOpts);
    futures.emplace_back(std::move(future));
  }

  auto cb = [details, options](std::vector<Try<network::Response>>&& results) mutable -> OperationResult {
    auto handler = [details](Result& result, VPackBuilder& builder, ShardID&,
                             VPackSlice answer) mutable -> void {
      if (answer.isObject()) {
        VPackSlice figures = answer.get("figures");
        // add to the total
        if (figures.isObject()) {
          aggregateClusterFigures(details, false, figures, builder);
        }
      } else {
        // didn't get the expected response
        result.reset(TRI_ERROR_INTERNAL);
      }
    };
    auto pre = [](Result&, VPackBuilder& builder) -> void {
      // initialize to empty object
      builder.add(VPackSlice::emptyObjectSlice());
    };
    return handleResponsesFromAllShards(options, results, handler, pre);
  };
  return futures::collectAll(std::move(futures)).thenValue(std::move(cb));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief counts number of documents in a coordinator, by shard
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> countOnCoordinator(transaction::Methods& trx,
                                                    std::string const& cname,
                                                    OperationOptions const& options) {
  std::vector<std::pair<std::string, uint64_t>> result;

  // Set a few variables needed for our work:
  ClusterFeature& feature = trx.vocbase().server().getFeature<ClusterFeature>();
  ClusterInfo& ci = feature.clusterInfo();

  std::string const& dbname = trx.vocbase().name();
  // First determine the collection ID from the name:
  std::shared_ptr<LogicalCollection> collinfo;
  collinfo = ci.getCollectionNT(dbname, cname);
  if (collinfo == nullptr) {
    return futures::makeFuture(OperationResult(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND, options));
  }

  std::shared_ptr<ShardMap> shardIds = collinfo->shardIds();
  const bool isManaged = trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED);
  if (isManaged) {
    Result res = ::beginTransactionOnAllLeaders(trx, *shardIds).get();
    if (res.fail()) {
      return futures::makeFuture(OperationResult(res, options));
    }
  }

  network::RequestOptions reqOpts;
  reqOpts.database = dbname;
  reqOpts.retryNotFound = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(shardIds->size());

  auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
  for (auto const& p : *shardIds) {
    network::Headers headers;
    ClusterTrxMethods::addTransactionHeader(trx, /*leader*/ p.second[0], headers);

    futures.emplace_back(network::sendRequestRetry(pool, "shard:" + p.first, fuerte::RestVerb::Get,
                                                   "/_api/collection/" + StringUtils::urlEncode(p.first) + "/count",
                                                   VPackBuffer<uint8_t>(), reqOpts, std::move(headers)));
  }

  auto cb = [options](std::vector<Try<network::Response>>&& results) mutable -> OperationResult {
    auto handler = [](Result& result, VPackBuilder& builder, ShardID& shardId,
                      VPackSlice answer) mutable -> void {
      if (answer.isObject()) {
        // add to the total
        VPackArrayBuilder array(&builder);
        array->add(VPackValue(shardId));
        array->add(VPackValue(Helper::getNumericValue<uint64_t>(
            answer, "count", 0)));
      } else {
        // didn't get the expected response
        result.reset(TRI_ERROR_INTERNAL);
      }
    };
    auto pre = [](Result&, VPackBuilder& builder) -> void {
      builder.openArray();
    };
    auto post = [](Result& result, VPackBuilder& builder) -> void {
      if (builder.isOpenArray()) {
        builder.close();
      } else {
        result.reset(TRI_ERROR_INTERNAL, "result was corrupted");
        builder.clear();
      }
    };
    return handleResponsesFromAllShards(options, results, handler, pre, post);
  };
  return futures::collectAll(std::move(futures)).thenValue(std::move(cb));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief gets the selectivity estimates from DBservers
////////////////////////////////////////////////////////////////////////////////

Result selectivityEstimatesOnCoordinator(ClusterFeature& feature, std::string const& dbname,
                                         std::string const& collname,
                                         std::unordered_map<std::string, double>& result,
                                         TransactionId tid) {
  // Set a few variables needed for our work:
  ClusterInfo& ci = feature.clusterInfo();

  result.clear();

  // First determine the collection ID from the name:
  std::shared_ptr<LogicalCollection> collinfo;
  collinfo = ci.getCollectionNT(dbname, collname);
  if (collinfo == nullptr) {
    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND};
  }
  std::shared_ptr<ShardMap> shards = collinfo->shardIds();

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.database = dbname;
  reqOpts.retryNotFound = true;
  reqOpts.skipScheduler = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(shards->size());

  std::string requestsUrl;
  for (auto const& p : *shards) {
    network::Headers headers;
    if (tid.isSet()) {
      headers.try_emplace(StaticStrings::TransactionId, std::to_string(tid.id()));
    }

    reqOpts.param("collection", p.first);
    futures.emplace_back(
        network::sendRequestRetry(pool, "shard:" + p.first, fuerte::RestVerb::Get,
                                  "/_api/index/selectivity",
                                  VPackBufferUInt8(), reqOpts, std::move(headers)));
  }

  // format of expected answer:
  // in `indexes` is a map that has keys in the format
  // s<shardid>/<indexid> and index information as value
  // {"code":200
  // ,"error":false
  // ,"indexes":{ "s10004/0"    : 1.0,
  //              "s10004/10005": 0.5
  //            }
  // }

  std::map<std::string, std::vector<double>> indexEstimates;
  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return {network::fuerteToArangoErrorCode(r), network::fuerteToArangoErrorMessage(r)};
    }

    VPackSlice answer = r.slice();
    if (!answer.isObject()) {
      return {TRI_ERROR_INTERNAL, "invalid response structure"};
    }

    if (answer.hasKey(StaticStrings::ErrorNum)) {
      Result res = network::resultFromBody(answer, TRI_ERROR_NO_ERROR);

      if (res.fail()) {
        return res;
      }
    }

    answer = answer.get("indexes");
    if (!answer.isObject()) {
      return {TRI_ERROR_INTERNAL, "invalid response structure for 'indexes'"};
    }

    // add to the total
    for (auto pair : VPackObjectIterator(answer, true)) {
      velocypack::StringRef shardIndexId(pair.key);
      auto split = std::find(shardIndexId.begin(), shardIndexId.end(), '/');
      if (split != shardIndexId.end()) {
        std::string index(split + 1, shardIndexId.end());
        double estimate = Helper::getNumericValue(pair.value, 0.0);
        indexEstimates[index].push_back(estimate);
      }
    }
  }

  auto aggregateIndexes = [](std::vector<double> const& vec) -> double {
    TRI_ASSERT(!vec.empty());
    double rv = std::accumulate(vec.begin(), vec.end(), 0.0);
    rv /= static_cast<double>(vec.size());
    return rv;
  };

  for (auto const& p : indexEstimates) {
    result[p.first] = aggregateIndexes(p.second);
  }

  return {};
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates one or many documents in a coordinator
///
/// In case of many documents (slice is a VPackArray) it will send to each
/// shard all the relevant documents for this shard only.
/// If one of them fails, this error is reported.
/// There is NO guarantee for the stored documents of all other shards, they may
/// be stored or not. All answers of these shards are dropped.
/// If we return with NO_ERROR it is guaranteed that all shards reported success
/// for their documents.
////////////////////////////////////////////////////////////////////////////////

Future<OperationResult> createDocumentOnCoordinator(transaction::Methods const& trx,
                                                    LogicalCollection& coll,
                                                    VPackSlice const slice,
                                                    arangodb::OperationOptions const& options) {
  const std::string collid = std::to_string(coll.id().id());

  // create vars used in this function
  bool const useMultiple = slice.isArray();  // insert more than one document
  CreateOperationCtx opCtx;
  opCtx.options = options;

  // create shard map
  if (useMultiple) {
    for (VPackSlice value : VPackArrayIterator(slice)) {
      int res = distributeBabyOnShards(opCtx, coll, value, options.isRestore);
      if (res != TRI_ERROR_NO_ERROR) {
        return makeFuture(OperationResult(res, options));
      }
    }
  } else {
    int res = distributeBabyOnShards(opCtx, coll, slice, options.isRestore);
    if (res != TRI_ERROR_NO_ERROR) {
      return makeFuture(OperationResult(res, options));
    }
  }

  Future<Result> f = makeFuture(Result());
  const bool isManaged = trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED);
  if (isManaged && opCtx.shardMap.size() > 1) {  // lazily begin transactions on leaders
    f = beginTransactionOnSomeLeaders(*trx.state(), coll, opCtx.shardMap);
  }

  return std::move(f).thenValue([=, &trx, &coll, opCtx(std::move(opCtx))]
                                (Result&& r) mutable -> Future<OperationResult> {
    if (r.fail()) {
      return OperationResult(std::move(r), options);
    }

    std::string const baseUrl = "/_api/document/";
    

    network::RequestOptions reqOpts;
    reqOpts.database = trx.vocbase().name();
    reqOpts.timeout = network::Timeout(CL_DEFAULT_LONG_TIMEOUT);
    reqOpts.retryNotFound = true;
    reqOpts.param(StaticStrings::WaitForSyncString, (options.waitForSync ? "true" : "false"))
           .param(StaticStrings::ReturnNewString, (options.returnNew ? "true" : "false"))
           .param(StaticStrings::ReturnOldString, (options.returnOld ? "true" : "false"))
           .param(StaticStrings::IsRestoreString, (options.isRestore ? "true" : "false"))
           .param(StaticStrings::KeepNullString, (options.keepNull ? "true" : "false"))
           .param(StaticStrings::MergeObjectsString, (options.mergeObjects ? "true" : "false"))
           .param(StaticStrings::SkipDocumentValidation, (options.validate ? "false" : "true"));
    if (options.isOverwriteModeSet()) {
      reqOpts.parameters.insert_or_assign(StaticStrings::OverwriteMode,
                                          OperationOptions::stringifyOverwriteMode(options.overwriteMode));
    }

    // Now prepare the requests:
    auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
    std::vector<Future<network::Response>> futures;
    futures.reserve(opCtx.shardMap.size());
    for (auto const& it : opCtx.shardMap) {
      VPackBuffer<uint8_t> reqBuffer;
      VPackBuilder reqBuilder(reqBuffer);

      if (!useMultiple) {
        TRI_ASSERT(it.second.size() == 1);
        auto idx = it.second.front();
        if (idx.second.empty()) {
          reqBuilder.add(slice);
        } else {
          reqBuilder.openObject();
          reqBuilder.add(StaticStrings::KeyString, VPackValue(idx.second));
          TRI_SanitizeObject(slice, reqBuilder);
          reqBuilder.close();
        }
      } else {
        reqBuilder.openArray(/*unindexed*/ true);
        for (auto const& idx : it.second) {
          if (idx.second.empty()) {
            reqBuilder.add(idx.first);
          } else {
            reqBuilder.openObject();
            reqBuilder.add(StaticStrings::KeyString, VPackValue(idx.second));
            TRI_SanitizeObject(idx.first, reqBuilder);
            reqBuilder.close();
          }
        }
        reqBuilder.close();
      }

      std::shared_ptr<ShardMap> shardIds = coll.shardIds();
      network::Headers headers;
      addTransactionHeaderForShard(trx, *shardIds, /*shard*/ it.first, headers);
      auto future =
          network::sendRequestRetry(pool, "shard:" + it.first, fuerte::RestVerb::Post,
                                    baseUrl + StringUtils::urlEncode(it.first),
                                    std::move(reqBuffer), reqOpts, std::move(headers));
      futures.emplace_back(std::move(future));
    }

    // Now compute the result
    if (!useMultiple) {  // single-shard fast track
      TRI_ASSERT(futures.size() == 1);

      auto cb = [options](network::Response&& res) -> OperationResult {
        if (res.error != fuerte::Error::NoError) {
          return OperationResult(network::fuerteToArangoErrorCode(res), options);
        }

        return network::clusterResultInsert(res.response->statusCode(),
                                            res.response->stealPayload(), options, {});
      };
      return std::move(futures[0]).thenValue(cb);
    }

    return futures::collectAll(std::move(futures))
        .thenValue([opCtx(std::move(opCtx))](std::vector<Try<network::Response>>&& results) -> OperationResult {
          return handleCRUDShardResponsesFast(network::clusterResultInsert, opCtx, results);
        });
  });
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

Future<OperationResult> removeDocumentOnCoordinator(arangodb::transaction::Methods& trx,
                                                    LogicalCollection& coll, VPackSlice const slice,
                                                    arangodb::OperationOptions const& options) {
  // Set a few variables needed for our work:

  // First determine the collection ID from the name:
  std::shared_ptr<ShardMap> shardIds = coll.shardIds();

  CrudOperationCtx opCtx;
  opCtx.options = options;
  const bool useMultiple = slice.isArray();

  bool canUseFastPath = true;
  if (useMultiple) {
    for (VPackSlice value : VPackArrayIterator(slice)) {
      int res = distributeBabyOnShards(opCtx, coll, value);
      if (res != TRI_ERROR_NO_ERROR) {
        canUseFastPath = false;
        break;
      }
    }
  } else {
    int res = distributeBabyOnShards(opCtx, coll, slice);
    if (res != TRI_ERROR_NO_ERROR) {
      canUseFastPath = false;
    }
  }
  // We sorted the shards correctly.

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.timeout = network::Timeout(CL_DEFAULT_LONG_TIMEOUT);
  reqOpts.retryNotFound = true;
  reqOpts.param(StaticStrings::WaitForSyncString, (options.waitForSync ? "true" : "false"))
         .param(StaticStrings::ReturnOldString, (options.returnOld ? "true" : "false"))
         .param(StaticStrings::IgnoreRevsString, (options.ignoreRevs ? "true" : "false"));

  const bool isManaged = trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED);

  if (canUseFastPath) {
    // All shard keys are known in all documents.
    // Contact all shards directly with the correct information.

    // lazily begin transactions on leaders
    Future<Result> f = makeFuture(Result());
    if (isManaged && opCtx.shardMap.size() > 1) {
      f = beginTransactionOnSomeLeaders(*trx.state(), coll, opCtx.shardMap);
    }

    return std::move(f).thenValue([=, &trx, opCtx(std::move(opCtx))]
                                  (Result&& r) mutable -> Future<OperationResult> {
      if (r.fail()) {
        return OperationResult(std::move(r), options);
      }

      // Now prepare the requests:
      auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
      std::vector<Future<network::Response>> futures;
      futures.reserve(opCtx.shardMap.size());

      for (auto const& it : opCtx.shardMap) {
        VPackBuffer<uint8_t> buffer;
        if (!useMultiple) {
          TRI_ASSERT(it.second.size() == 1);
          buffer.append(slice.begin(), slice.byteSize());
        } else {
          VPackBuilder reqBuilder(buffer);
          reqBuilder.openArray(/*unindexed*/true);
          for (VPackSlice value : it.second) {
            reqBuilder.add(value);
          }
          reqBuilder.close();
        }

        network::Headers headers;
        addTransactionHeaderForShard(trx, *shardIds, /*shard*/ it.first, headers);
        futures.emplace_back(network::sendRequestRetry(
            pool, "shard:" + it.first, fuerte::RestVerb::Delete,
            "/_api/document/" + StringUtils::urlEncode(it.first),
            std::move(buffer), reqOpts, std::move(headers)));
      }

      // Now listen to the results:
      if (!useMultiple) {
        TRI_ASSERT(futures.size() == 1);
        auto cb = [options](network::Response&& res) -> OperationResult {
          if (res.error != fuerte::Error::NoError) {
            return OperationResult(network::fuerteToArangoErrorCode(res), options);
          }
          return network::clusterResultDelete(res.response->statusCode(),
                                              res.response->stealPayload(), options, {});
        };
        return std::move(futures[0]).thenValue(cb);
      }

      return futures::collectAll(std::move(futures))
          .thenValue([opCtx = std::move(opCtx)](std::vector<Try<network::Response>>&& results) -> OperationResult {
            return handleCRUDShardResponsesFast(network::clusterResultDelete, opCtx, results);
          });
    });

  }

  // Not all shard keys are known in all documents.
  // We contact all shards with the complete body and ignore NOT_FOUND

  // lazily begin transactions on leaders
  Future<Result> f = makeFuture(Result());
  if (isManaged && shardIds->size() > 1) {
    f = ::beginTransactionOnAllLeaders(trx, *shardIds);
  }

  return std::move(f).thenValue([=, &trx](Result&& r) mutable -> Future<OperationResult> {
    if (r.fail()) {
      return OperationResult(r, options);
    }

    // We simply send the body to all shards and await their results.
    // As soon as we have the results we merge them in the following way:
    // For 1 .. slice.length()
    //    for res : allResults
    //      if res != NOT_FOUND => insert this result. skip other results
    //    end
    //    if (!skipped) => insert NOT_FOUND

    auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
    std::vector<Future<network::Response>> futures;
    futures.reserve(shardIds->size());

    const size_t expectedLen = useMultiple ? slice.length() : 0;
    VPackBuffer<uint8_t> buffer;
    buffer.append(slice.begin(), slice.byteSize());

    for (auto const& shardServers : *shardIds) {
      ShardID const& shard = shardServers.first;
      network::Headers headers;
      addTransactionHeaderForShard(trx, *shardIds, shard, headers);
      futures.emplace_back(
          network::sendRequestRetry(pool, "shard:" + shard, fuerte::RestVerb::Delete,
                                    "/_api/document/" + StringUtils::urlEncode(shard),
                                    /*cannot move*/ buffer,
                                    reqOpts, std::move(headers)));
    }

    return futures::collectAll(std::move(futures))
    .thenValue([=](std::vector<Try<network::Response>>&& responses) mutable -> OperationResult {
      return ::handleCRUDShardResponsesSlow(network::clusterResultDelete, expectedLen,
                                            std::move(options), responses);
    });
  });
}

////////////////////////////////////////////////////////////////////////////////
/// @brief truncate a cluster collection on a coordinator
////////////////////////////////////////////////////////////////////////////////

futures::Future<OperationResult> truncateCollectionOnCoordinator(
    transaction::Methods& trx, std::string const& collname, OperationOptions const& options) {
  Result res;
  // Set a few variables needed for our work:
  ClusterInfo& ci = trx.vocbase().server().getFeature<ClusterFeature>().clusterInfo();

  // First determine the collection ID from the name:
  std::shared_ptr<LogicalCollection> collinfo;
  collinfo = ci.getCollectionNT(trx.vocbase().name(), collname);
  if (collinfo == nullptr) {
    return futures::makeFuture(
        OperationResult(res.reset(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND), options));
  }

  // Some stuff to prepare cluster-intern requests:
  // We have to contact everybody:
  std::shared_ptr<ShardMap> shardIds = collinfo->shardIds();

  // lazily begin transactions on all leader shards
  if (trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED)) {
    res = ::beginTransactionOnAllLeaders(trx, *shardIds).get();
    if (res.fail()) {
      return futures::makeFuture(OperationResult(res, options));
    }
  }

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.timeout = network::Timeout(600.0);
  reqOpts.retryNotFound = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(shardIds->size());

  auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
  for (auto const& p : *shardIds) {
    // handler expects valid velocypack body (empty object minimum)
    VPackBuffer<uint8_t> buffer;
    VPackBuilder builder(buffer);
    builder.openObject();
    builder.close();

    network::Headers headers;
    addTransactionHeaderForShard(trx, *shardIds, /*shard*/ p.first, headers);
    auto future =
        network::sendRequestRetry(pool, "shard:" + p.first, fuerte::RestVerb::Put,
                                  "/_api/collection/" + p.first + "/truncate",
                                  std::move(buffer), reqOpts, std::move(headers));
    futures.emplace_back(std::move(future));
  }

  auto cb = [options](std::vector<Try<network::Response>>&& results) -> OperationResult {
    return handleResponsesFromAllShards(
        options, results,
        [](Result& result, VPackBuilder&, ShardID&, VPackSlice answer) -> void {
          if (Helper::getBooleanValue(answer, StaticStrings::Error, false)) {
            result = network::resultFromBody(answer, TRI_ERROR_NO_ERROR);
          }
        });
  };
  return futures::collectAll(std::move(futures)).thenValue(std::move(cb));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

Future<OperationResult> getDocumentOnCoordinator(transaction::Methods& trx,
                                                 LogicalCollection& coll, VPackSlice slice,
                                                 OperationOptions const& options) {
  // Set a few variables needed for our work:

  const std::string collid = std::to_string(coll.id().id());
  std::shared_ptr<ShardMap> shardIds = coll.shardIds();

  // If _key is the one and only sharding attribute, we can do this quickly,
  // because we can easily determine which shard is responsible for the
  // document. Otherwise we have to contact all shards and ask them to
  // delete the document. All but one will not know it.
  // Now find the responsible shard(s)

  CrudOperationCtx opCtx;
  opCtx.options = options;
  const bool useMultiple = slice.isArray();

  bool canUseFastPath = true;
  if (useMultiple) {
    for (VPackSlice value : VPackArrayIterator(slice)) {
      int res = distributeBabyOnShards(opCtx, coll, value);
      if (res != TRI_ERROR_NO_ERROR) {
        canUseFastPath = false;
        break;
      }
    }
  } else {
    int res = distributeBabyOnShards(opCtx, coll, slice);
    if (res != TRI_ERROR_NO_ERROR) {
      canUseFastPath = false;
    }
  }

  // lazily begin transactions on leaders
  const bool isManaged = trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED);

  // Some stuff to prepare cluster-internal requests:

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.retryNotFound = true;
  reqOpts.param(StaticStrings::IgnoreRevsString, (options.ignoreRevs ? "true" : "false"));

  fuerte::RestVerb restVerb;
  if (!useMultiple) {
    restVerb = options.silent ? fuerte::RestVerb::Head : fuerte::RestVerb::Get;
  } else {
    restVerb = fuerte::RestVerb::Put;
    if (options.silent) {
      reqOpts.param(StaticStrings::SilentString, "true");
    }
    reqOpts.param("onlyget", "true");
  }

  if (canUseFastPath) {
    // All shard keys are known in all documents.
    // Contact all shards directly with the correct information.

    Future<Result> f = makeFuture(Result());
    if (isManaged && opCtx.shardMap.size() > 1) {  // lazily begin the transaction
      f = beginTransactionOnSomeLeaders(*trx.state(), coll, opCtx.shardMap);
    }

    return std::move(f).thenValue([=, &trx, opCtx(std::move(opCtx))]
                                  (Result&& r) mutable -> Future<OperationResult> {
      if (r.fail()) {
        return OperationResult(std::move(r), options);
      }

      // Now prepare the requests:
      auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
      std::vector<Future<network::Response>> futures;
      futures.reserve(opCtx.shardMap.size());

      for (auto const& it : opCtx.shardMap) {
        network::Headers headers;
        addTransactionHeaderForShard(trx, *shardIds, /*shard*/ it.first, headers);
        std::string url;
        VPackBuffer<uint8_t> buffer;

        if (!useMultiple) {
          TRI_ASSERT(it.second.size() == 1);

          if (!options.ignoreRevs && slice.hasKey(StaticStrings::RevString)) {
            headers.try_emplace("if-match", slice.get(StaticStrings::RevString).copyString());
          }
          VPackSlice keySlice = slice;
          if (slice.isObject()) {
            keySlice = slice.get(StaticStrings::KeyString);
          }
          VPackStringRef ref = keySlice.stringRef();
          // We send to single endpoint
          url.append("/_api/document/").append(StringUtils::urlEncode(it.first)).push_back('/');
          url.append(StringUtils::urlEncode(ref.data(), ref.length()));
        } else {
          // We send to Babies endpoint
          url.append("/_api/document/").append(StringUtils::urlEncode(it.first));

          VPackBuilder builder(buffer);
          builder.openArray(/*unindexed*/true);
          for (auto const& value : it.second) {
            builder.add(value);
          }
          builder.close();
        }
        futures.emplace_back(
            network::sendRequestRetry(pool, "shard:" + it.first, restVerb,
                                      std::move(url), std::move(buffer), reqOpts,
                                      std::move(headers)));
      }

      // Now compute the result
      if (!useMultiple) {  // single-shard fast track
        TRI_ASSERT(futures.size() == 1);
        return std::move(futures[0]).thenValue([options](network::Response res) -> OperationResult {
          if (res.error != fuerte::Error::NoError) {
            return OperationResult(network::fuerteToArangoErrorCode(res), options);
          }
          return network::clusterResultDocument(res.response->statusCode(),
                                                res.response->stealPayload(), options, {});
        });
      }

      return futures::collectAll(std::move(futures))
          .thenValue([opCtx = std::move(opCtx)](std::vector<Try<network::Response>>&& results) {
            return handleCRUDShardResponsesFast(network::clusterResultDocument, opCtx, results);
          });
    });

  }

  // Not all shard keys are known in all documents.
  // We contact all shards with the complete body and ignore NOT_FOUND

  if (isManaged) {  // lazily begin the transaction
    Result res = ::beginTransactionOnAllLeaders(trx, *shardIds).get();
    if (res.fail()) {
      return makeFuture(OperationResult(res, options));
    }
  }

  // Now prepare the requests:
  std::vector<Future<network::Response>> futures;
  futures.reserve(shardIds->size());

  auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
  const size_t expectedLen = useMultiple ? slice.length() : 0;
  if (!useMultiple) {
    VPackStringRef const key(slice.isObject() ? slice.get(StaticStrings::KeyString) : slice);

    const bool addMatch = !options.ignoreRevs && slice.hasKey(StaticStrings::RevString);
    for (auto const& shardServers : *shardIds) {
      ShardID const& shard = shardServers.first;

      network::Headers headers;
      addTransactionHeaderForShard(trx, *shardIds, shard, headers);
      if (addMatch) {
        headers.try_emplace("if-match", slice.get(StaticStrings::RevString).copyString());
      }

      futures.emplace_back(network::sendRequestRetry(
          pool, "shard:" + shard, restVerb,
          "/_api/document/" + StringUtils::urlEncode(shard) + "/" +
              StringUtils::urlEncode(key.data(), key.size()),
          VPackBuffer<uint8_t>(), reqOpts, std::move(headers)));
    }
  } else {
    VPackBuffer<uint8_t> buffer;
    buffer.append(slice.begin(), slice.byteSize());
    for (auto const& shardServers : *shardIds) {
      ShardID const& shard = shardServers.first;
      network::Headers headers;
      addTransactionHeaderForShard(trx, *shardIds, shard, headers);
      futures.emplace_back(
          network::sendRequestRetry(pool, "shard:" + shard, restVerb,
                                    "/_api/document/" + StringUtils::urlEncode(shard),
                                    /*cannot move*/ buffer, reqOpts, std::move(headers)));
    }
  }

  return futures::collectAll(std::move(futures))
      .thenValue([=](std::vector<Try<network::Response>>&& responses) mutable -> OperationResult {
        return ::handleCRUDShardResponsesSlow(network::clusterResultDocument, expectedLen,
                                              std::move(options), responses);
      });
}

/// @brief fetch edges from TraverserEngines
///        Contacts all TraverserEngines placed
///        on the DBServers for the given list
///        of vertex _id's.
///        All non-empty and non-cached results
///        of DBServers will be inserted in the
///        datalake. Slices used in the result
///        point to content inside of this lake
///        only and do not run out of scope unless
///        the lake is cleared.

Result fetchEdgesFromEngines(transaction::Methods& trx,
                             graph::ClusterTraverserCache& travCache,
                             traverser::TraverserOptions const* opts,
                             arangodb::velocypack::StringRef vertexId,
                             size_t depth,
                             std::vector<VPackSlice>& result) {
  auto const* engines = travCache.engines();
  std::unordered_map<arangodb::velocypack::StringRef, VPackSlice>& cache =
      travCache.cache();
  auto& datalake = travCache.datalake();
  size_t& filtered = travCache.filteredDocuments();
  size_t& read = travCache.insertedDocuments();

  // TODO map id => ServerID if possible
  // And go fast-path
  transaction::BuilderLeaser leased(&trx);
  leased->openObject();
  leased->add("depth", VPackValue(depth));
  leased->add("keys", VPackValuePair(vertexId.data(), vertexId.length(), VPackValueType::String));
  leased->add(VPackValue("variables"));
  {
    leased->openArray();
    opts->serializeVariables(*(leased.get()));
    leased->close();
  }
  leased->close();

  std::string const url = "/_internal/traverser/edge/";

  auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.skipScheduler = true; // hack to avoid scheduler queue

  std::vector<Future<network::Response>> futures;
  futures.reserve(engines->size());

  for (auto const& engine : *engines) {
    futures.emplace_back(
        network::sendRequestRetry(pool, "server:" + engine.first, fuerte::RestVerb::Put,
                                  url + StringUtils::itoa(engine.second),
                                  leased->bufferRef(), reqOpts));
  }

  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return network::fuerteToArangoErrorCode(r);
    }

    auto payload = r.response->stealPayload();
    VPackSlice resSlice(payload->data());
    if (!resSlice.isObject()) {
      // Response has invalid format
      return TRI_ERROR_HTTP_CORRUPTED_JSON;
    }

    Result res = network::resultFromBody(resSlice, TRI_ERROR_NO_ERROR);
    if (res.fail()) {
      return res;
    }
    filtered += Helper::getNumericValue<size_t>(resSlice, "filtered", 0);
    read += Helper::getNumericValue<size_t>(resSlice, "readIndex", 0);

    VPackSlice edges = resSlice.get("edges");
    bool allCached = true;

    for (VPackSlice e : VPackArrayIterator(edges)) {
      VPackSlice id = e.get(StaticStrings::IdString);
      if (!id.isString()) {
        // invalid id type
        LOG_TOPIC("a23b5", ERR, Logger::GRAPHS)
            << "got invalid edge id type: " << id.typeName();
        continue;
      }
      arangodb::velocypack::StringRef idRef(id);
      auto resE = cache.emplace(idRef, e);
      if (resE.second) {
        // This edge is not yet cached.
        allCached = false;
        result.emplace_back(e);
      } else {
        result.emplace_back(resE.first->second);
      }
    }
    if (!allCached) {
      datalake.emplace_back(std::move(payload));
    }
  }
  return {};
}

/// @brief fetch edges from TraverserEngines
///        Contacts all TraverserEngines placed
///        on the DBServers for the given list
///        of vertex _id's.
///        All non-empty and non-cached results
///        of DBServers will be inserted in the
///        datalake. Slices used in the result
///        point to content inside of this lake
///        only and do not run out of scope unless
///        the lake is cleared.

Result fetchEdgesFromEngines(transaction::Methods& trx,
                             std::unordered_map<ServerID, aql::EngineId> const* engines,
                             VPackSlice const vertexId, bool backward,
                             std::unordered_map<arangodb::velocypack::StringRef, VPackSlice>& cache,
                             std::vector<VPackSlice>& result,
                             std::vector<std::shared_ptr<VPackBufferUInt8>>& datalake, size_t& read) {
  // TODO map id => ServerID if possible
  // And go fast-path

  // This function works for one specific vertex
  // or for a list of vertices.
  TRI_ASSERT(vertexId.isString() || vertexId.isArray());
  transaction::BuilderLeaser leased(&trx);
  leased->openObject();
  leased->add("backward", VPackValue(backward));
  leased->add("keys", vertexId);
  leased->close();

  std::string const url = "/_internal/traverser/edge/";

  auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.skipScheduler = true; // hack to avoid scheduler queue

  std::vector<Future<network::Response>> futures;
  futures.reserve(engines->size());

  for (auto const& engine : *engines) {
    futures.emplace_back(
        network::sendRequestRetry(pool, "server:" + engine.first, fuerte::RestVerb::Put,
                                  url + StringUtils::itoa(engine.second),
                                  leased->bufferRef(), reqOpts));
  }

  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return network::fuerteToArangoErrorCode(r);
    }

    auto payload = r.response->stealPayload();
    VPackSlice resSlice(payload->data());
    if (!resSlice.isObject()) {
      // Response has invalid format
      return TRI_ERROR_HTTP_CORRUPTED_JSON;
    }
    Result res = network::resultFromBody(resSlice, TRI_ERROR_NO_ERROR);
    if (res.fail()) {
      return res;
    }
    read += Helper::getNumericValue<size_t>(resSlice, "readIndex", 0);

    bool allCached = true;
    VPackSlice edges = resSlice.get("edges");
    for (VPackSlice e : VPackArrayIterator(edges)) {
      VPackSlice id = e.get(StaticStrings::IdString);
      if (!id.isString()) {
        // invalid id type
        LOG_TOPIC("da49d", ERR, Logger::GRAPHS)
            << "got invalid edge id type: " << id.typeName();
        continue;
      }
      arangodb::velocypack::StringRef idRef(id);
      auto resE = cache.emplace(idRef, e);
      if (resE.second) {
        // This edge is not yet cached.
        allCached = false;
        result.emplace_back(e);
      } else {
        result.emplace_back(resE.first->second);
      }
    }
    if (!allCached) {
      datalake.emplace_back(std::move(payload));
    }
  }
  return {};
}

/// @brief fetch vertices from TraverserEngines
///        Contacts all TraverserEngines placed
///        on the DBServers for the given list
///        of vertex _id's.
///        If any server responds with a document
///        it will be inserted into the result.
///        If no server responds with a document
///        a 'null' will be inserted into the result.

void fetchVerticesFromEngines(
    transaction::Methods& trx,
    std::unordered_map<ServerID, aql::EngineId> const* engines,
    std::unordered_set<arangodb::velocypack::StringRef>& vertexIds,
    std::unordered_map<arangodb::velocypack::StringRef, VPackSlice>& result,
    std::vector<std::shared_ptr<VPackBufferUInt8>>& datalake,
    bool forShortestPath) {

  // TODO map id => ServerID if possible
  // And go fast-path

  // slow path, sharding not deducable from _id
  transaction::BuilderLeaser leased(&trx);
  leased->openObject();
  leased->add("keys", VPackValue(VPackValueType::Array));
  for (auto const& v : vertexIds) {
    leased->add(VPackValuePair(v.data(), v.length(), VPackValueType::String));
  }
  leased->close();  // 'keys' Array
  leased->close();  // base object

  std::string const url = "/_internal/traverser/vertex/";

  auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.skipScheduler = true; // hack to avoid scheduler queue

  std::vector<Future<network::Response>> futures;
  futures.reserve(engines->size());

  for (auto const& engine : *engines) {
    futures.emplace_back(
        network::sendRequestRetry(pool, "server:" + engine.first, fuerte::RestVerb::Put,
                                  url + StringUtils::itoa(engine.second),
                                  leased->bufferRef(), reqOpts));
  }

  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      THROW_ARANGO_EXCEPTION(network::fuerteToArangoErrorCode(r));
    }

    auto payload = r.response->stealPayload();
    VPackSlice resSlice(payload->data());
    if (!resSlice.isObject()) {
      // Response has invalid format
      THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_CORRUPTED_JSON);
    }
    if (r.response->statusCode() != fuerte::StatusOK) {
      // We have an error case here. Throw it.
      THROW_ARANGO_EXCEPTION(network::resultFromBody(resSlice, TRI_ERROR_INTERNAL));
    }

    bool cached = false;
    for (auto pair : VPackObjectIterator(resSlice, /*sequential*/true)) {
      arangodb::velocypack::StringRef key(pair.key);
      if (vertexIds.erase(key) == 0) {
        // We either found the same vertex twice,
        // or found a vertex we did not request.
        // Anyways something somewhere went seriously wrong
        THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_GOT_CONTRADICTING_ANSWERS);
      }
      TRI_ASSERT(result.find(key) == result.end());
      if (!cached) {
        datalake.emplace_back(std::move(payload));
        cached = true;
      }
      // Protected by datalake
      result.try_emplace(key, pair.value);
    }
  }

  if (!forShortestPath) {
    // Fill everything we did not find with NULL
    for (auto const& v : vertexIds) {
      result.try_emplace(v, VPackSlice::nullSlice());
    }
    vertexIds.clear();
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief modify a document in a coordinator
////////////////////////////////////////////////////////////////////////////////

Future<OperationResult> modifyDocumentOnCoordinator(
    transaction::Methods& trx, LogicalCollection& coll, VPackSlice const& slice,
    arangodb::OperationOptions const& options, bool const isPatch) {
  // Set a few variables needed for our work:

  // First determine the collection ID from the name:
  std::shared_ptr<ShardMap> shardIds = coll.shardIds();

  // We have a fast path and a slow path. The fast path only asks one shard
  // to do the job and the slow path asks them all and expects to get
  // "not found" from all but one shard. We have to cover the following
  // cases:
  //   isPatch == false    (this is a "replace" operation)
  //     Here, the complete new document is given, we assume that we
  //     can read off the responsible shard, therefore can use the fast
  //     path, this is always true if _key is the one and only sharding
  //     attribute, however, if there is any other sharding attribute,
  //     it is possible that the user has changed the values in any of
  //     them, in that case we will get a "not found" or a "sharding
  //     attributes changed answer" in the fast path. In the first case
  //     we have to delegate to the slow path.
  //   isPatch == true     (this is an "update" operation)
  //     In this case we might or might not have all sharding attributes
  //     specified in the partial document given. If _key is the one and
  //     only sharding attribute, it is always given, if not all sharding
  //     attributes are explicitly given (at least as value `null`), we must
  //     assume that the fast path cannot be used. If all sharding attributes
  //     are given, we first try the fast path, but might, as above,
  //     have to use the slow path after all.

  ShardID shardID;

  CrudOperationCtx opCtx;
  opCtx.options = options;
  const bool useMultiple = slice.isArray();

  bool canUseFastPath = true;
  if (useMultiple) {
    for (VPackSlice value : VPackArrayIterator(slice)) {
      int res = distributeBabyOnShards(opCtx, coll, value);
      if (res != TRI_ERROR_NO_ERROR) {
        if (!isPatch) { // shard keys cannot be changed, error out early
          return makeFuture(OperationResult(res, options));
        }
        canUseFastPath = false;
        break;
      }
    }
  } else {
    int res = distributeBabyOnShards(opCtx, coll, slice);
    if (res != TRI_ERROR_NO_ERROR) {
      if (!isPatch) { // shard keys cannot be changed, error out early
        return makeFuture(OperationResult(res, options));
      }
      canUseFastPath = false;
    }
  }

  // Some stuff to prepare cluster-internal requests:

  network::RequestOptions reqOpts;
  reqOpts.database = trx.vocbase().name();
  reqOpts.timeout = network::Timeout(CL_DEFAULT_LONG_TIMEOUT);
  reqOpts.retryNotFound = true;
  reqOpts.param(StaticStrings::WaitForSyncString, (options.waitForSync ? "true" : "false"))
         .param(StaticStrings::IgnoreRevsString, (options.ignoreRevs ? "true" : "false"))
         .param(StaticStrings::SkipDocumentValidation, (options.validate ? "false" : "true"))
         .param(StaticStrings::IsRestoreString, (options.isRestore ? "true" : "false"));

  fuerte::RestVerb restVerb;
  if (isPatch) {
    restVerb = fuerte::RestVerb::Patch;
    if (!options.keepNull) {
      reqOpts.param(StaticStrings::KeepNullString, "false");
    }
    reqOpts.param(StaticStrings::MergeObjectsString, (options.mergeObjects ? "true" : "false"));
  } else {
    restVerb = fuerte::RestVerb::Put;
  }
  if (options.returnNew) {
    reqOpts.param(StaticStrings::ReturnNewString, "true");
  }
  if (options.returnOld) {
    reqOpts.param(StaticStrings::ReturnOldString, "true");
  }

  const bool isManaged = trx.state()->hasHint(transaction::Hints::Hint::GLOBAL_MANAGED);

  if (canUseFastPath) {
    // All shard keys are known in all documents.
    // Contact all shards directly with the correct information.

    Future<Result> f = makeFuture(Result());
    if (isManaged && opCtx.shardMap.size() > 1) {  // lazily begin transactions on leaders
      f = beginTransactionOnSomeLeaders(*trx.state(), coll, opCtx.shardMap);
    }

    return std::move(f).thenValue([=, &trx, opCtx(std::move(opCtx))](Result&& r) mutable -> Future<OperationResult> {
      if (r.fail()) { // bail out
        return OperationResult(r, opCtx.options);
      }

      // Now prepare the requests:
      auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
      std::vector<Future<network::Response>> futures;
      futures.reserve(opCtx.shardMap.size());

      for (auto const& it : opCtx.shardMap) {
        std::string url;
        VPackBuffer<uint8_t> buffer;

        if (!useMultiple) {
          TRI_ASSERT(it.second.size() == 1);
          TRI_ASSERT(slice.isObject());
          VPackStringRef const ref(slice.get(StaticStrings::KeyString));
          // We send to single endpoint
          url = "/_api/document/" + StringUtils::urlEncode(it.first) + "/" +
                StringUtils::urlEncode(ref.data(), ref.length());
          buffer.append(slice.begin(), slice.byteSize());

        } else {
          // We send to Babies endpoint
          url = "/_api/document/" + StringUtils::urlEncode(it.first);

          VPackBuilder builder(buffer);
          builder.clear();
          builder.openArray(/*unindexed*/true);
          for (auto const& value : it.second) {
            builder.add(value);
          }
          builder.close();
        }

        network::Headers headers;
        addTransactionHeaderForShard(trx, *shardIds, /*shard*/ it.first, headers);
        futures.emplace_back(
            network::sendRequestRetry(pool, "shard:" + it.first, restVerb,
                                      std::move(url), std::move(buffer),
                                      reqOpts, std::move(headers)));
      }

      // Now listen to the results:
      if (!useMultiple) {
        TRI_ASSERT(futures.size() == 1);
        auto cb = [options](network::Response&& res) -> OperationResult {
          if (res.error != fuerte::Error::NoError) {
            return OperationResult(network::fuerteToArangoErrorCode(res), options);
          }
          return network::clusterResultModify(res.response->statusCode(),
                                              res.response->stealPayload(), options, {});
        };
        return std::move(futures[0]).thenValue(cb);
      }

      return futures::collectAll(std::move(futures))
          .thenValue([opCtx = std::move(opCtx)](std::vector<Try<network::Response>>&& results) -> OperationResult {
            return handleCRUDShardResponsesFast(network::clusterResultModify, opCtx, results);
          });
    });
  }

  // Not all shard keys are known in all documents.
  // We contact all shards with the complete body and ignore NOT_FOUND

  Future<Result> f = makeFuture(Result());
  if (isManaged && shardIds->size() > 1) {  // lazily begin the transaction
    f = ::beginTransactionOnAllLeaders(trx, *shardIds);
  }

  return std::move(f).thenValue([=, &trx](Result&&) mutable -> Future<OperationResult> {
    auto* pool = trx.vocbase().server().getFeature<NetworkFeature>().pool();
    std::vector<Future<network::Response>> futures;
    futures.reserve(shardIds->size());

    const size_t expectedLen = useMultiple ? slice.length() : 0;
    VPackBuffer<uint8_t> buffer;
    buffer.append(slice.begin(), slice.byteSize());

    for (auto const& shardServers : *shardIds) {
      ShardID const& shard = shardServers.first;
      network::Headers headers;
      addTransactionHeaderForShard(trx, *shardIds, shard, headers);

      std::string url;
      if (!useMultiple) { // send to single API
        VPackStringRef const key(slice.get(StaticStrings::KeyString));
        url = "/_api/document/" + StringUtils::urlEncode(shard) + "/" +
              StringUtils::urlEncode(key.data(), key.size());
      } else {
        url = "/_api/document/" + StringUtils::urlEncode(shard);
      }
      futures.emplace_back(
          network::sendRequestRetry(pool, "shard:" + shard, restVerb,
                                    std::move(url), /*cannot move*/ buffer,
                                    reqOpts, std::move(headers)));
    }

    return futures::collectAll(std::move(futures))
    .thenValue([=](std::vector<Try<network::Response>>&& responses) mutable -> OperationResult {
      return ::handleCRUDShardResponsesSlow(network::clusterResultModify, expectedLen,
                                            std::move(options), responses);
    });
  });
}

////////////////////////////////////////////////////////////////////////////////
/// @brief flush Wal on all DBservers
////////////////////////////////////////////////////////////////////////////////

int flushWalOnAllDBServers(ClusterFeature& feature, bool waitForSync,
                           bool waitForCollector, double maxWaitTime) {
  ClusterInfo& ci = feature.clusterInfo();

  std::vector<ServerID> DBservers = ci.getCurrentDBServers();

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true; // hack to avoid scheduler queue
  reqOpts.param(StaticStrings::WaitForSyncString, (waitForSync ? "true" : "false"))
         .param("waitForCollector", (waitForCollector ? "true" : "false"));
  if (maxWaitTime >= 0.0) {
    reqOpts.param("maxWaitTime", std::to_string(maxWaitTime));
  }

  std::vector<Future<network::Response>> futures;
  futures.reserve(DBservers.size());

  VPackBufferUInt8 buffer;
  buffer.append(VPackSlice::noneSlice().begin(), 1); // necessary for some reason
  for (std::string const& server : DBservers) {
    futures.emplace_back(
        network::sendRequestRetry(pool, "server:" + server, fuerte::RestVerb::Put,
                                  "/_admin/wal/flush", buffer, reqOpts));
  }

  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return network::fuerteToArangoErrorCode(r);
    }
    if (r.response->statusCode() != fuerte::StatusOK) {
      int code = network::errorCodeFromBody(r.slice());
      if (code != TRI_ERROR_NO_ERROR) {
        return code;
      }
    }
  }
  return TRI_ERROR_NO_ERROR;
}
 
/// @brief compact the database on all DB servers
Result compactOnAllDBServers(ClusterFeature& feature,
                             bool changeLevel, bool compactBottomMostLevel) {
  ClusterInfo& ci = feature.clusterInfo();

  std::vector<ServerID> DBservers = ci.getCurrentDBServers();

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.timeout = network::Timeout(3600);
  reqOpts.skipScheduler = true; // hack to avoid scheduler queue
  reqOpts.param("changeLevel", (changeLevel ? "true" : "false"))
         .param("compactBottomMostLevel", (compactBottomMostLevel ? "true" : "false"));

  std::vector<Future<network::Response>> futures;
  futures.reserve(DBservers.size());

  VPackBufferUInt8 buffer;
  VPackSlice const s = VPackSlice::emptyObjectSlice();
  buffer.append(s.start(), s.byteSize());
  for (std::string const& server : DBservers) {
    futures.emplace_back(
        network::sendRequestRetry(pool, "server:" + server, fuerte::RestVerb::Put,
                                  "/_admin/compact", buffer, reqOpts));
  }

  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return {network::fuerteToArangoErrorCode(r), network::fuerteToArangoErrorMessage(r)};
    }
    if (r.response->statusCode() != fuerte::StatusOK) {
      return network::resultFromBody(r.slice(), TRI_ERROR_INTERNAL);
    }
  }
  return {};
}

#ifndef USE_ENTERPRISE
std::vector<std::shared_ptr<LogicalCollection>> ClusterMethods::createCollectionOnCoordinator(
    TRI_vocbase_t& vocbase, velocypack::Slice parameters, bool ignoreDistributeShardsLikeErrors,
    bool waitForSyncReplication, bool enforceReplicationFactor,
    bool isNewDatabase, std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike) {
  TRI_ASSERT(parameters.isArray());
  // Collections are temporary collections object that undergoes integrity checks
  // etc. It is not used anywhere and will be cleaned up after this call.
  std::vector<std::shared_ptr<LogicalCollection>> cols;
  for (VPackSlice p : VPackArrayIterator(parameters)) {
    cols.emplace_back(std::make_shared<LogicalCollection>(vocbase, p, true));
  }

  // Persist collection will return the real object.
  auto& feature = vocbase.server().getFeature<ClusterFeature>();
  auto usableCollectionPointers =
      persistCollectionsInAgency(feature, cols, ignoreDistributeShardsLikeErrors,
                                 waitForSyncReplication, enforceReplicationFactor,
                                 isNewDatabase, colToDistributeShardsLike);
  TRI_ASSERT(usableCollectionPointers.size() == cols.size());
  return usableCollectionPointers;
}
#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief Persist collection in Agency and trigger shard creation process
////////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<LogicalCollection>> ClusterMethods::persistCollectionsInAgency(
    ClusterFeature& feature, std::vector<std::shared_ptr<LogicalCollection>>& collections,
    bool ignoreDistributeShardsLikeErrors, bool waitForSyncReplication,
    bool enforceReplicationFactor, bool isNewDatabase,
    std::shared_ptr<LogicalCollection> const& colToDistributeLike) {
  TRI_ASSERT(!collections.empty());
  if (collections.empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "Trying to create an empty list of collections on coordinator.");
  }

  double const realTimeout = ClusterInfo::getTimeout(240.0);
  double const endTime = TRI_microtime() + realTimeout;

  // We have at least one, take this collection's DB name
  // (if there are multiple collections to create, the assumption is that
  // all collections have the same database name - ArangoDB does not
  // support cross-database operations and they cannot be triggered by
  // users)
  auto const dbName = collections[0]->vocbase().name();
  ClusterInfo& ci = feature.clusterInfo();

  std::vector<ClusterCollectionCreationInfo> infos;

  while (true) {
    infos.clear();

    ci.loadCurrentDBServers();
    std::vector<std::string> dbServers = ci.getCurrentDBServers();
    infos.reserve(collections.size());

    std::vector<std::shared_ptr<VPackBuffer<uint8_t>>> vpackData;
    vpackData.reserve(collections.size());

    for (auto& col : collections) {
      // We can only serve on Database at a time with this call.
      // We have the vocbase context around this calls anyways, so this is safe.
      TRI_ASSERT(col->vocbase().name() == dbName);
      std::string distributeShardsLike = col->distributeShardsLike();
      std::vector<std::string> avoid = col->avoidServers();
      std::shared_ptr<std::unordered_map<std::string, std::vector<std::string>>> shards = nullptr;

      if (!distributeShardsLike.empty()) {
        std::shared_ptr<LogicalCollection> myColToDistributeLike;

        if (colToDistributeLike != nullptr) {
          myColToDistributeLike = colToDistributeLike;
        } else {
          CollectionNameResolver resolver(col->vocbase());
          myColToDistributeLike = resolver.getCollection(distributeShardsLike);
          if (myColToDistributeLike == nullptr) {
            THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_UNKNOWN_DISTRIBUTESHARDSLIKE,
                                           "Collection not found: " + distributeShardsLike + " in database " + col->vocbase().name());
          }
        }

        shards = CloneShardDistribution(ci, col, myColToDistributeLike);
      } else {
        // system collections should never enforce replicationfactor
        // to allow them to come up with 1 dbserver
        if (col->system()) {
          enforceReplicationFactor = false;
        }

        size_t replicationFactor = col->replicationFactor();
        size_t writeConcern = col->writeConcern();
        size_t numberOfShards = col->numberOfShards();

        // the default behavior however is to bail out and inform the user
        // that the requested replicationFactor is not possible right now
        if (dbServers.size() < replicationFactor) {
          TRI_ASSERT(writeConcern <= replicationFactor);
          // => (dbServers.size() < writeConcern) is granted
          LOG_TOPIC("9ce2e", DEBUG, Logger::CLUSTER)
              << "Do not have enough DBServers for requested replicationFactor,"
              << " nrDBServers: " << dbServers.size()
              << " replicationFactor: " << replicationFactor;
          if (enforceReplicationFactor) {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS);
          }
        }

        if (!avoid.empty()) {
          // We need to remove all servers that are in the avoid list
          if (dbServers.size() - avoid.size() < replicationFactor) {
            LOG_TOPIC("03682", DEBUG, Logger::CLUSTER)
                << "Do not have enough DBServers for requested "
                   "replicationFactor,"
                << " (after considering avoid list),"
                << " nrDBServers: " << dbServers.size()
                << " replicationFactor: " << replicationFactor
                << " avoid list size: " << avoid.size();
            // Not enough DBServers left
            THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS);
          }
          dbServers.erase(std::remove_if(dbServers.begin(), dbServers.end(),
                                         [&](const std::string& x) {
                                           return std::find(avoid.begin(), avoid.end(),
                                                            x) != avoid.end();
                                         }),
                          dbServers.end());
        }
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(dbServers.begin(), dbServers.end(), g);
        shards = DistributeShardsEvenly(ci, numberOfShards, replicationFactor,
                                      dbServers, !col->system());
      } // if - distributeShardsLike.empty()


      if (shards->empty() && !col->isSmart()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "no database servers found in cluster");
      }

      col->setShardMap(shards);

      std::unordered_set<std::string> const ignoreKeys{
          "allowUserKeys", "cid",     "globallyUniqueId", "count",
          "planId",        "version", "objectId"};
      col->setStatus(TRI_VOC_COL_STATUS_LOADED);
      VPackBuilder velocy =
          col->toVelocyPackIgnore(ignoreKeys, LogicalDataSource::Serialization::List);

      auto const serverState = ServerState::instance();
      infos.emplace_back(ClusterCollectionCreationInfo{
          std::to_string(col->id().id()), col->numberOfShards(),
          col->replicationFactor(), col->writeConcern(), waitForSyncReplication,
          velocy.slice(), serverState->getId(), serverState->getRebootId()});
      vpackData.emplace_back(velocy.steal());
    } // for col : collections

    // pass in the *endTime* here, not a timeout!
    Result res = ci.createCollectionsCoordinator(dbName, infos, endTime,
                                                 isNewDatabase, colToDistributeLike);

    if (res.ok()) {
      // success! exit the loop and go on
      break;
    }

    if (res.is(TRI_ERROR_CLUSTER_CREATE_COLLECTION_PRECONDITION_FAILED)) {
      // special error code indicating that storing the updated plan in the
      // agency didn't succeed, and that we should try again

      // sleep for a while
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      if (TRI_microtime() > endTime) {
        // timeout expired
        THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_TIMEOUT);
      }

      if (feature.server().isStopping()) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_SHUTTING_DOWN);
      }

      // try in next iteration with an adjusted plan change attempt
      continue;

    } else {
      // any other error
      THROW_ARANGO_EXCEPTION(res);
    }
  }

  //ci.loadPlan();

  // Produce list of shared_ptr wrappers

  std::vector<std::shared_ptr<LogicalCollection>> usableCollectionPointers;
  usableCollectionPointers.reserve(infos.size());

  // quick exit if new database
  if (isNewDatabase) {
    for (auto const& col : collections) {
      usableCollectionPointers.emplace_back(col);
    }
  } else {
    for (auto const& i : infos) {
      auto c = ci.getCollection(dbName, i.collectionID);
      TRI_ASSERT(c.get() != nullptr);
      // We never get a nullptr here because an exception is thrown if the
      // collection does not exist. Also, the create collection should have
      // failed before.
      usableCollectionPointers.emplace_back(std::move(c));
    }
  }
  return usableCollectionPointers;
}  

std::string const apiStr("/_admin/backup/");

arangodb::Result hotBackupList(network::ConnectionPool* pool,
                               std::vector<ServerID> const& dbServers, VPackSlice const idSlice,
                               std::unordered_map<std::string, BackupMeta>& hotBackups,
                               VPackBuilder& plan) {
  TRI_ASSERT(pool);
  hotBackups.clear();
  TRI_ASSERT(idSlice.isArray() || idSlice.isString() || idSlice.isNone());
  
  std::map<std::string, std::vector<BackupMeta>> dbsBackups;

  VPackBufferUInt8 body;
  VPackBuilder b(body);
  b.openObject();
  if (!idSlice.isNone()) {
    b.add("id", idSlice);
  }
  b.close();
  
  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;

  std::string const url = apiStr + "list";

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());
  for (auto const& dbServer : dbServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  size_t nrGood = 0;
  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();
    if (!r.ok()) {
      continue;
    }
    if (r.response->checkStatus({fuerte::StatusOK,
                                 fuerte::StatusCreated,
                                 fuerte::StatusAccepted,
                                 fuerte::StatusNoContent})) {
      nrGood++;
    }
  }


  LOG_TOPIC("410a1", DEBUG, Logger::BACKUP) << "Got " << nrGood << " of "
  << futures.size() << " lists of local backups";

  // Any error if no id presented
  if (idSlice.isNone() && nrGood < futures.size()) {
    return arangodb::Result(
      TRI_ERROR_HOT_BACKUP_DBSERVERS_AWOL,
      std::string("not all db servers could be reached for backup listing"));
  }

  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();
    if (!r.ok()) {
      continue;
    }
    VPackSlice resSlice = r.slice();
    if (!resSlice.isObject()) {
      // Response has invalid format
      return arangodb::Result(TRI_ERROR_HTTP_CORRUPTED_JSON,
                              std::string("result to list request to ") +
                                  r.destination + " not an object");
    }

    if (resSlice.get(StaticStrings::Error).getBoolean()) {
      return arangodb::Result(static_cast<int>(resSlice.get(StaticStrings::ErrorNum).getNumber<uint64_t>()),
                              resSlice.get(StaticStrings::ErrorMessage).copyString());
    }

    if (!resSlice.hasKey("result") || !resSlice.get("result").isObject()) {
      return arangodb::Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                              std::string("invalid response ") +
                                  resSlice.toJson() + "from " + r.destination);
    }

    resSlice = resSlice.get("result");

    if (!resSlice.hasKey("list") || !resSlice.get("list").isObject()) {
      continue;
    }

    if (!idSlice.isNone() && plan.slice().isNone()) {
      if (!resSlice.hasKey("agency-dump") || !resSlice.get("agency-dump").isArray() ||
          resSlice.get("agency-dump").length() != 1) {
        return arangodb::Result(TRI_ERROR_HTTP_NOT_FOUND,
                                std::string("result ") + resSlice.toJson() +
                                    " is missing agency dump");
      }
      plan.add(resSlice.get("agency-dump")[0]);
    }

    for (auto backup : VPackObjectIterator(resSlice.get("list"))) {
      ResultT<BackupMeta> meta = BackupMeta::fromSlice(backup.value);
      if (meta.ok()) {
        dbsBackups[backup.key.copyString()].push_back(std::move(meta.get()));
      }
    }
  }

  for (auto& i : dbsBackups) {
    // check if the backup is on all dbservers
    bool valid = true;

    // check here that the backups are all made with the same version
    std::string version;
    size_t totalSize = 0;
    size_t totalFiles = 0;

    for (BackupMeta const& meta : i.second) {
      if (version.empty()) {
        version = meta._version;
      } else {
        if (version != meta._version) {
          LOG_TOPIC("aaaaa", WARN, Logger::BACKUP)
              << "Backup " << meta._id
              << " has different versions accross dbservers: " << version
              << " and " << meta._version;
          valid = false;
          break;
        }
      }
      totalSize += meta._sizeInBytes;
      totalFiles += meta._nrFiles;
    }

    if (valid) {  // backup is identical on all servers
      BackupMeta& front = i.second.front();
      front._sizeInBytes = totalSize;
      front._nrFiles = totalFiles;
      front._serverId = "";  // makes no sense for whole cluster
      front._isAvailable = i.second.size() == dbServers.size() && i.second.size() == front._nrDBServers;
      front._nrPiecesPresent = static_cast<unsigned int>(i.second.size());
      hotBackups.insert(std::make_pair(front._id, front));
    }
  }

  return arangodb::Result();
}

/**
 * @brief Match existing servers with those in the backup
 *
 * @param  agencyDump
 * @param  my own DB server list
 * @param  Result container
 */
arangodb::Result matchBackupServers(VPackSlice const agencyDump,
                                    std::vector<ServerID> const& dbServers,
                                    std::map<ServerID, ServerID>& match) {
  std::vector<std::string> ap{"arango", "Plan", "DBServers"};

  if (!agencyDump.hasKey(ap)) {
    return Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                  "agency dump must contain key DBServers");
  }
  auto planServers = agencyDump.get(ap);

  return matchBackupServersSlice(planServers, dbServers, match);
}

arangodb::Result matchBackupServersSlice(VPackSlice const planServers,
                                         std::vector<ServerID> const& dbServers,
                                         std::map<ServerID, ServerID>& match) {
  // LOG_TOPIC("711d8", DEBUG, Logger::BACKUP) << "matching db servers between snapshot: " <<
  //  planServers.toJson() << " and this cluster's db servers " << dbServers;

  if (!planServers.isObject()) {
    return Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                  "agency dump's arango.Plan.DBServers must be object");
  }

  if (dbServers.size() < planServers.length()) {
    return Result(TRI_ERROR_BACKUP_TOPOLOGY,
                  std::string("number of db servers in the backup (") +
                      std::to_string(planServers.length()) +
                      ") and in this cluster (" +
                      std::to_string(dbServers.size()) + ") do not match");
  }

  // Clear match container
  match.clear();

  // Local copy of my servers
  std::unordered_set<std::string> localCopy;
  std::copy(dbServers.begin(), dbServers.end(),
            std::inserter(localCopy, localCopy.end()));

  // Skip all direct matching names in pair and remove them from localCopy
  std::unordered_set<std::string>::iterator it;
  for (auto planned : VPackObjectIterator(planServers)) {
    auto const plannedStr = planned.key.copyString();
    if ((it = std::find(localCopy.begin(), localCopy.end(), plannedStr)) !=
        localCopy.end()) {
      localCopy.erase(it);
    } else {
      match.try_emplace(plannedStr, std::string());
    }
  }
  // match all remaining
  auto it2 = localCopy.begin();
  for (auto& m : match) {
    m.second = *it2++;
  }


  LOG_TOPIC("a201e", DEBUG, Logger::BACKUP) << "DB server matches: " << match;

  return arangodb::Result();
}

arangodb::Result controlMaintenanceFeature(network::ConnectionPool* pool,
                                           std::string const& command,
                                           std::string const& backupId,
                                           std::vector<ServerID> const& dbServers) {

  VPackBufferUInt8 body;
  VPackBuilder builder(body);
  {
    VPackObjectBuilder b(&builder);
    builder.add("execute", VPackValue(command));
    builder.add("reason", VPackValue("backup"));
    builder.add("duration", VPackValue(30));
    builder.add("id", VPackValue(backupId));
  }

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());
  std::string const url = "/_admin/actions";

  for (auto const& dbServer : dbServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  LOG_TOPIC("3d080", DEBUG, Logger::BACKUP)
      << "Attempting to execute " << command << " maintenance features for hot backup id "
      << backupId << " using " << builder.toJson();

  // Now listen to the results:
  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return arangodb::Result(network::fuerteToArangoErrorCode(r),
                              std::string(
                                  "Communication error while executing " + command + " maintenance on ") +
                                  r.destination);
    }
    TRI_ASSERT(r.response != nullptr);

    VPackSlice resSlice = r.slice();
    if (!resSlice.isObject() || !resSlice.hasKey(StaticStrings::Error) ||
        !resSlice.get(StaticStrings::Error).isBoolean()) {
      // Response has invalid format
      return arangodb::Result(TRI_ERROR_HTTP_CORRUPTED_JSON,
                              std::string("result of executing " + command + " request to maintenance feature on ") +
                                  r.destination + " is invalid");
    }

    if (resSlice.get(StaticStrings::Error).getBoolean()) {
      return arangodb::Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                              std::string("failed to execute " + command + " on maintenance feature for ") +
                                  backupId + " on server " + r.destination);
    }

    LOG_TOPIC("d7e7c", DEBUG, Logger::BACKUP)
        << "maintenance is paused on " << r.destination;
  }

  return arangodb::Result();
}

arangodb::Result restoreOnDBServers(network::ConnectionPool* pool,
                                    std::string const& backupId,
                                    std::vector<std::string> const& dbServers,
                                    std::string& previous, bool ignoreVersion) {

  VPackBufferUInt8 body;
  VPackBuilder builder(body);
  {
    VPackObjectBuilder o(&builder);
    builder.add("id", VPackValue(backupId));
    builder.add("ignoreVersion", VPackValue(ignoreVersion));
  }

  std::string const url = apiStr + "restore";

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());

  for (auto const& dbServer : dbServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  LOG_TOPIC("37960", DEBUG, Logger::BACKUP) << "Restoring backup " << backupId;

  // Now listen to the results:
  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      // oh-oh cluster is in a bad state
      return arangodb::Result(network::fuerteToArangoErrorCode(r),
                              std::string("Communication error list backups on ") + r.destination);
    }
    TRI_ASSERT(r.response != nullptr);

    VPackSlice resSlice = r.slice();
    if (!resSlice.isObject()) {
      // Response has invalid format
      return arangodb::Result(TRI_ERROR_HTTP_CORRUPTED_JSON,
                              std::string("result to restore request ") +
                                  r.destination + "not an object");
    }

    if (!resSlice.hasKey(StaticStrings::Error) || !resSlice.get(StaticStrings::Error).isBoolean() ||
        resSlice.get(StaticStrings::Error).getBoolean()) {
      return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              std::string("failed to restore ") + backupId +
                                  " on server " + r.destination + ": " +
                                  resSlice.toJson());
    }

    if (!resSlice.hasKey("result") || !resSlice.get("result").isObject()) {
      return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              std::string("failed to restore ") + backupId +
                                  " on server " + r.destination +
                                  " as response is missing result object: " +
                                  resSlice.toJson());
    }

    auto result = resSlice.get("result");

    if (!result.hasKey("previous") || !result.get("previous").isString()) {
      return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              std::string("failed to restore ") + backupId +
                                  " on server " + r.destination);
    }

    previous = result.get("previous").copyString();
    LOG_TOPIC("9a5c4", DEBUG, Logger::BACKUP)
        << "received failsafe name " << previous << " from db server " << r.destination;
  }

  LOG_TOPIC("755a2", DEBUG, Logger::BACKUP) << "Restored " << backupId << " successfully";

  return arangodb::Result();
}

arangodb::Result applyDBServerMatchesToPlan(VPackSlice const plan,
                                            std::map<ServerID, ServerID> const& matches,
                                            VPackBuilder& newPlan) {
  std::function<void(VPackSlice const, std::map<ServerID, ServerID> const&)> replaceDBServer;

  replaceDBServer = [&newPlan, &replaceDBServer](VPackSlice const s,
                                                 std::map<ServerID, ServerID> const& matches) {
    if (s.isObject()) {
      VPackObjectBuilder o(&newPlan);
      for (auto it : VPackObjectIterator(s)) {
        newPlan.add(it.key);
        replaceDBServer(it.value, matches);
      }
    } else if (s.isArray()) {
      VPackArrayBuilder a(&newPlan);
      for (VPackSlice it : VPackArrayIterator(s)) {
        replaceDBServer(it, matches);
      }
    } else {
      bool swapped = false;
      if (s.isString()) {
        for (auto const& match : matches) {
          if (s.isString() && s.isEqualString(match.first)) {
            newPlan.add(VPackValue(match.second));
            swapped = true;
            break;
          }
        }
      }
      if (!swapped) {
        newPlan.add(s);
      }
    }
  };

  replaceDBServer(plan, matches);

  return arangodb::Result();
}

arangodb::Result hotRestoreCoordinator(ClusterFeature& feature, VPackSlice const payload,
                                       VPackBuilder& report) {
  // 1. Find local backup with id
  //    - fail if not found
  // 2. Match db servers
  //    - fail if not matching
  // 3. Check if they have according backup with backupId
  //    - fail if not
  // 4. Stop maintenance feature on all db servers
  // 5. a. Replay agency
  //    b. Initiate DB server restores
  // 6. Wait until all dbservers up again and good
  //    - fail if not

  VPackSlice idSlice;
  if (!payload.isObject() || !(idSlice = payload.get("id")).isString()) {
    events::RestoreHotbackup("", TRI_ERROR_BAD_PARAMETER);
    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        "restore payload must be an object with string attribute 'id'");
  }
  TRI_ASSERT(idSlice.isString());

  bool ignoreVersion =
      payload.hasKey("ignoreVersion") && payload.get("ignoreVersion").isTrue();

  std::string const backupId = idSlice.copyString();
  VPackBuilder plan;
  ClusterInfo& ci = feature.clusterInfo();

  auto const& nf = feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (!pool) {
    // shutdown, leave here
    return TRI_ERROR_SHUTTING_DOWN;
  }

  std::vector<ServerID> dbServers = ci.getCurrentDBServers();
  std::unordered_map<std::string, BackupMeta> list;

  auto result = hotBackupList(pool, dbServers, idSlice, list, plan);
  if (!result.ok()) {
    LOG_TOPIC("ed4dd", ERR, Logger::BACKUP)
        << "failed to find backup " << backupId
        << " on all db servers: " << result.errorMessage();
    events::RestoreHotbackup(backupId, result.errorNumber());
    return result;
  }
  if (list.empty()) {
    events::RestoreHotbackup(backupId, TRI_ERROR_HTTP_NOT_FOUND);
    return arangodb::Result(TRI_ERROR_HTTP_NOT_FOUND,
      "result is missing backup list");
  }

  if (plan.slice().isNone()) {
    LOG_TOPIC("54b9a", ERR, Logger::BACKUP)
        << "failed to find agency dump for " << backupId
        << " on any db server: " << result.errorMessage();
    events::RestoreHotbackup(backupId, result.errorNumber());
    return result;
  }

  TRI_ASSERT(list.size() == 1);
  BackupMeta& meta = list.begin()->second;
  if (!meta._isAvailable) {
    LOG_TOPIC("ed4df", ERR, Logger::BACKUP)
        << "backup not available" << backupId;
    events::RestoreHotbackup(backupId, TRI_ERROR_HOT_RESTORE_INTERNAL);
    return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              "backup not available for restore");
  }

  // Check if the version matches the current version
  if (!ignoreVersion) {
    TRI_ASSERT(list.size() == 1);
    using arangodb::methods::Version;
    using arangodb::methods::VersionResult;
#ifdef USE_ENTERPRISE
    // Will never be called in Community Edition
    bool autoUpgradeNeeded;   // not actually used
    if (!RocksDBHotBackup::versionTestRestore(meta._version, autoUpgradeNeeded)) {
      events::RestoreHotbackup(backupId, TRI_ERROR_HOT_RESTORE_INTERNAL);
      return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              "Version mismatch");
    }
#endif
  }

  // Match my db servers to those in the backups's agency dump
  std::map<ServerID, ServerID> matches;
  result = matchBackupServers(plan.slice(), dbServers, matches);
  if (!result.ok()) {
    LOG_TOPIC("5a746", ERR, Logger::BACKUP)
        << "failed to match db servers: " << result.errorMessage();
    events::RestoreHotbackup(backupId, result.errorNumber());
    return result;
  }

  // Apply matched servers to create new plan, if any matches to be done,
  // else just take
  VPackBuilder newPlan;
  if (!matches.empty()) {
    result = applyDBServerMatchesToPlan(plan.slice(), matches, newPlan);
    if (!result.ok()) {
      events::RestoreHotbackup(backupId, result.errorNumber());
      return result;
    }
  }

  // Pause maintenance feature everywhere, fail, if not succeeded everywhere
  result = controlMaintenanceFeature(pool, "pause", backupId, dbServers);
  if (!result.ok()) {
    events::RestoreHotbackup(backupId, result.errorNumber());
    return result;
  }

  // Enact new plan upon the agency
  result = (matches.empty()) ? ci.agencyReplan(plan.slice())
                             : ci.agencyReplan(newPlan.slice());
  if (!result.ok()) {
    result = controlMaintenanceFeature(pool, "proceed", backupId, dbServers);
    events::RestoreHotbackup(backupId, result.errorNumber());
    return result;
  }

  // Now I will have to wait for the plan to trickle down
  std::this_thread::sleep_for(std::chrono::seconds(5));

  // We keep the currently registered timestamps in Current/ServersRegistered,
  // such that we can wait until all have reregistered and are up:

  ci.loadCurrentDBServers();
  auto const preServersKnown = ci.rebootIds();

  // Restore all db servers
  std::string previous;
  result = restoreOnDBServers(pool, backupId, dbServers, previous, ignoreVersion);
  if (!result.ok()) {  // This is disaster!
    events::RestoreHotbackup(backupId, result.errorNumber());
    return result;
  }

  // no need to keep connections to shut-down servers, they auto close when unused
  if (pool) {
    pool->drainConnections();
  }

  auto startTime = std::chrono::steady_clock::now();
  while (true) {  // will be left by a timeout
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (feature.server().isStopping()) {
      events::RestoreHotbackup(backupId, TRI_ERROR_HOT_RESTORE_INTERNAL);
      return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              "Shutdown of coordinator!");
    }
    if (std::chrono::steady_clock::now() - startTime > std::chrono::minutes(15)) {
      events::RestoreHotbackup(backupId, TRI_ERROR_HOT_RESTORE_INTERNAL);
      return arangodb::Result(TRI_ERROR_HOT_RESTORE_INTERNAL,
                              "Not all DBservers came back in time!");
    }
    ci.loadCurrentDBServers();
    auto const postServersKnown = ci.rebootIds();
    if (ci.getCurrentDBServers().size() < dbServers.size()) {
      LOG_TOPIC("8dce7", INFO, Logger::BACKUP) << "Waiting for all db servers to return";
      continue;
    }

    // Check timestamps of all dbservers:
    size_t good = 0;  // Count restarted servers
    for (auto const& dbs : dbServers) {
      if (postServersKnown.at(dbs) != preServersKnown.at(dbs)) {
        ++good;
      }
    }
    LOG_TOPIC("8dc7e", INFO, Logger::BACKUP)
        << "Backup restore: So far " << good << "/" << dbServers.size()
        << " dbServers have reregistered.";
    if (good >= dbServers.size()) {
      break;
    }
  }

  {
    VPackObjectBuilder o(&report);
    report.add("previous", VPackValue(previous));
    report.add("isCluster", VPackValue(true));
  }
  events::RestoreHotbackup(backupId, TRI_ERROR_NO_ERROR);
  return arangodb::Result();
}

std::vector<std::string> lockPath = std::vector<std::string>{"result", "lockId"};

arangodb::Result lockDBServerTransactions(network::ConnectionPool* pool,
                                          std::string const& backupId,
                                          std::vector<ServerID> const& dbServers,
                                          double const& lockWait,
                                          std::vector<ServerID>& lockedServers) {
  using namespace std::chrono;

  // Make sure all db servers have the backup with backup Id

  std::string const url = apiStr + "lock";

  VPackBufferUInt8 body;
  VPackBuilder lock(body);
  {
    VPackObjectBuilder o(&lock);
    lock.add("id", VPackValue(backupId));
    lock.add("timeout", VPackValue(lockWait));
    lock.add("unlockTimeout", VPackValue(5.0 + lockWait));
  }

  LOG_TOPIC("707ed", DEBUG, Logger::BACKUP)
      << "Trying to acquire global transaction locks using body " << lock.toJson();

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;
  reqOpts.timeout = network::Timeout(lockWait + 5.0);

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());

  for (auto const& dbServer : dbServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  // Now listen to the results and report the aggregated final result:
  arangodb::Result finalRes(TRI_ERROR_NO_ERROR);
  auto reportError = [&](int c, std::string const& m) {
    if (finalRes.ok()) {
      finalRes = arangodb::Result(c, m);
    } else {
      // If we see at least one TRI_ERROR_LOCAL_LOCK_FAILED it is a failure
      // if all errors are TRI_ERROR_LOCK_TIMEOUT, then we report this and
      // this will lead to a retry:
      if (finalRes.errorNumber() == TRI_ERROR_LOCAL_LOCK_FAILED) {
        c = TRI_ERROR_LOCAL_LOCK_FAILED;
      }
      finalRes = arangodb::Result(c, finalRes.errorMessage() + ", " + m);
    }
  };
  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      reportError(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("Communication error locking transactions on ")
                  + r.destination);
      continue;
    }
    TRI_ASSERT(r.response != nullptr);
    VPackSlice slc = r.slice();

    if (!slc.isObject() || !slc.hasKey(StaticStrings::Error) || !slc.get(StaticStrings::Error).isBoolean()) {
      reportError(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("invalid response from ") + r.destination +
                  " when trying to freeze transactions for hot backup " +
                  backupId + ": " + slc.toJson());
      continue;
    }

    if (slc.get(StaticStrings::Error).getBoolean()) {
      LOG_TOPIC("f4b8f", DEBUG, Logger::BACKUP)
          << "failed to acquire lock from " << r.destination << ": " << slc.toJson();
      auto errorNum = slc.get(StaticStrings::ErrorNum).getNumber<int>();
      if (errorNum == TRI_ERROR_LOCK_TIMEOUT) {
        reportError(errorNum, slc.get(StaticStrings::ErrorMessage).copyString());
        continue;
      }
      reportError(TRI_ERROR_LOCAL_LOCK_FAILED,
          std::string("lock was denied from ") + r.destination +
          " when trying to check for lockId for hot backup " + backupId +
          ": " + slc.toJson());
      continue;
    }

    if (!slc.hasKey(lockPath) || !slc.get(lockPath).isNumber() ||
        !slc.hasKey("result") || !slc.get("result").isObject()) {
      reportError(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("invalid response from ") + r.destination +
                  " when trying to check for lockId for hot backup " +
                  backupId + ": " + slc.toJson());
      continue;
    }

    uint64_t lockId = 0;
    try {
      lockId = slc.get(lockPath).getNumber<uint64_t>();
      LOG_TOPIC("14457", DEBUG, Logger::BACKUP)
          << "acquired lock from " << r.destination << " for backupId "
          << backupId << " with lockId " << lockId;
    } catch (std::exception const& e) {
      reportError(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("invalid response from ") + r.destination +
                  " when trying to get lockId for hot backup " + backupId +
                  ": " + slc.toJson() + ", msg: " + e.what());
      continue;
    }

    lockedServers.push_back(r.destination.substr(strlen("server:"), std::string::npos));
  }

  if (finalRes.ok()) {
    LOG_TOPIC("c1869", DEBUG, Logger::BACKUP)
        << "acquired transaction locks on all db servers";
  }

  return finalRes;
}

arangodb::Result unlockDBServerTransactions(network::ConnectionPool* pool,
                                            std::string const& backupId,
                                            std::vector<ServerID> const& lockedServers) {
  using namespace std::chrono;

  // Make sure all db servers have the backup with backup Id

  std::string const url = apiStr + "unlock";

  VPackBufferUInt8 body;
  VPackBuilder lock(body);
  {
    VPackObjectBuilder o(&lock);
    lock.add("id", VPackValue(backupId));
  }

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(lockedServers.size());

  for (auto const& dbServer : lockedServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  std::ignore = futures::collectAll(futures).get();

  LOG_TOPIC("2ba8f", DEBUG, Logger::BACKUP)
      << "best try to kill all locks on db servers";

  return arangodb::Result();
}

std::vector<std::string> idPath{"result", "id"};

arangodb::Result hotBackupDBServers(network::ConnectionPool* pool,
                                    std::string const& backupId, std::string const& timeStamp,
                                    std::vector<ServerID> dbServers,
                                    VPackSlice agencyDump, bool force,
                                    BackupMeta& meta) {

  VPackBufferUInt8 body;
  VPackBuilder builder(body);
  {
    VPackObjectBuilder b(&builder);
    builder.add("label", VPackValue(backupId));
    builder.add("agency-dump", agencyDump);
    builder.add("timestamp", VPackValue(timeStamp));
    builder.add("allowInconsistent", VPackValue(force));
    builder.add("nrDBServers", VPackValue(dbServers.size()));
  }

  std::string const url = apiStr + "create";

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());

  for (auto const& dbServer : dbServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  LOG_TOPIC("478ef", DEBUG, Logger::BACKUP) << "Inquiring about backup " << backupId;

  // Now listen to the results:
  size_t totalSize = 0;
  size_t totalFiles = 0;
  std::vector<std::string> secretHashes;
  std::string version;
  bool sizeValid = true;
  for (Future<network::Response>& f : futures) {
    network::Response const& r = f.get();

    if (r.fail()) {
      return arangodb::Result(network::fuerteToArangoErrorCode(r),
          std::string("Communication error list backups on ") + r.destination);
    }
    TRI_ASSERT(r.response != nullptr);

    VPackSlice resSlice = r.slice();
    if (!resSlice.isObject() || !resSlice.hasKey("result")) {
      // Response has invalid format
      return arangodb::Result(TRI_ERROR_HTTP_CORRUPTED_JSON,
                              std::string("result to take snapshot on ") +
                                  r.destination + " not an object or has no 'result' attribute");
    }
    resSlice = resSlice.get("result");

    // TODO: shouldn't the id be checked ?
    VPackSlice value = resSlice.get(BackupMeta::ID);
    if (!value.isString()) {
      LOG_TOPIC("6240a", ERR, Logger::BACKUP)
          << "DB server " << r.destination << "is missing backup " << backupId;
      return arangodb::Result(TRI_ERROR_FILE_NOT_FOUND,
                              std::string("no backup with id ") + backupId +
                                  " on server " + r.destination);
    }
    
    value = resSlice.get(BackupMeta::SECRETHASH);
    if (value.isArray()) {
      for (VPackSlice hash : VPackArrayIterator(value)) {
        if (hash.isString()) {
          secretHashes.push_back(hash.copyString());
        }
      }
    }

    if (resSlice.hasKey(BackupMeta::SIZEINBYTES)) {
      totalSize += Helper::getNumericValue<size_t>(resSlice, BackupMeta::SIZEINBYTES, 0);
    } else {
      sizeValid = false;
    }
    if (resSlice.hasKey(BackupMeta::NRFILES)) {
      totalFiles += Helper::getNumericValue<size_t>(resSlice, BackupMeta::NRFILES, 0);
    } else {
      sizeValid = false;
    }
    if (version.empty() && resSlice.hasKey(BackupMeta::VERSION)) {
      VPackSlice verSlice = resSlice.get(BackupMeta::VERSION);
      if (verSlice.isString()) {
        version = verSlice.copyString();
      }
    }

    LOG_TOPIC("b370d", DEBUG, Logger::BACKUP) << r.destination << " created local backup "
                                              << resSlice.get(BackupMeta::ID).copyString();
  }
  
  // remove duplicate hashes
  std::sort(secretHashes.begin(), secretHashes.end());
  secretHashes.erase(std::unique(secretHashes.begin(), secretHashes.end()), secretHashes.end());

  if (sizeValid) {
    meta = BackupMeta(backupId, version, timeStamp, secretHashes, totalSize, totalFiles,
                      static_cast<unsigned int>(dbServers.size()), "", force);
  } else {
    meta = BackupMeta(backupId, version, timeStamp, secretHashes, 0, 0,
                      static_cast<unsigned int>(dbServers.size()), "", force);
    LOG_TOPIC("54265", WARN, Logger::BACKUP)
      << "Could not determine total size of backup with id '" << backupId
      << "'!";
  }
  LOG_TOPIC("5c5e9", DEBUG, Logger::BACKUP) << "Have created backup " << backupId;

  return arangodb::Result();
}

/**
 * @brief delete all backups with backupId from the db servers
 */
arangodb::Result removeLocalBackups(network::ConnectionPool* pool,
                                    std::string const& backupId,
                                    std::vector<ServerID> const& dbServers,
                                    std::vector<std::string>& deleted) {

  VPackBufferUInt8 body;
  VPackBuilder builder(body);
  {
    VPackObjectBuilder b(&builder);
    builder.add("id", VPackValue(backupId));
  }

  std::string const url = apiStr + "delete";

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());

  for (auto const& dbServer : dbServers) {
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body, reqOpts));
  }

  LOG_TOPIC("33e85", DEBUG, Logger::BACKUP) << "Deleting backup " << backupId;

  size_t notFoundCount = 0;

  // Now listen to the results:
  for (Future<network::Response>& f : futures) {
     network::Response const& r = f.get();

     if (r.fail()) {
      return arangodb::Result(network::fuerteToArangoErrorCode(r),
                              std::string(
                                  "Communication error while deleting backup") +
                                  backupId + " on " + r.destination);
    }
    TRI_ASSERT(r.response != nullptr);

    VPackSlice resSlice = r.slice();
    if (!resSlice.isObject()) {
      // Response has invalid format
      return arangodb::Result(TRI_ERROR_HTTP_CORRUPTED_JSON,
                              std::string("failed to remove backup from ") +
                                  r.destination + ", result not an object");
    }

    if (!resSlice.hasKey(StaticStrings::Error) || !resSlice.get(StaticStrings::Error).isBoolean() ||
        resSlice.get(StaticStrings::Error).getBoolean()) {
      int64_t errorNum = resSlice.get(StaticStrings::ErrorNum).getNumber<int64_t>();

      if (errorNum == TRI_ERROR_FILE_NOT_FOUND) {
        notFoundCount += 1;
        continue;
      }

      std::string errorMsg = std::string("failed to delete backup ") +
                             backupId + " on " + r.destination + ":" +
                             resSlice.get(StaticStrings::ErrorMessage).copyString() + " (" +
                             std::to_string(errorNum) + ")";

      LOG_TOPIC("9b94f", ERR, Logger::BACKUP) << errorMsg;
      return arangodb::Result(static_cast<int>(errorNum), errorMsg);
    }
  }

  LOG_TOPIC("1b318", DEBUG, Logger::BACKUP)
      << "removeLocalBackups: notFoundCount = " << notFoundCount << " "
      << dbServers.size();

  if (notFoundCount == dbServers.size()) {
    return arangodb::Result(TRI_ERROR_HTTP_NOT_FOUND,
                            "Backup " + backupId + " not found.");
  }

  deleted.emplace_back(backupId);
  LOG_TOPIC("04e97", DEBUG, Logger::BACKUP) << "Have located and deleted " << backupId;

  return arangodb::Result();
}

std::vector<std::string> const versionPath =
    std::vector<std::string>{"arango", "Plan", "Version"};

arangodb::Result hotbackupAsyncLockDBServersTransactions(network::ConnectionPool* pool,
  std::string const& backupId,
  std::vector<ServerID> const& dbServers, double const& lockWait,
  std::unordered_map<std::string, std::string> &dbserverLockIds)
{

  std::string const url = apiStr + "lock";

  VPackBufferUInt8 body;
  VPackBuilder lock(body);
  {
    VPackObjectBuilder o(&lock);
    lock.add("id", VPackValue(backupId));
    lock.add("timeout", VPackValue(lockWait));
    lock.add("unlockTimeout", VPackValue(5.0 + lockWait));
  }

  LOG_TOPIC("707ee", DEBUG, Logger::BACKUP)
      << "Trying to acquire async global transaction locks using body " << lock.toJson();

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;
  reqOpts.timeout = network::Timeout(lockWait + 5.0);

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbServers.size());

  for (auto const& dbServer : dbServers) {
    network::Headers headers;
    headers.emplace(StaticStrings::Async, "store");
    futures.emplace_back(network::sendRequest(pool, "server:" + dbServer,
                                              fuerte::RestVerb::Post, url, body,
                                              reqOpts, std::move(headers)));
  }

  // Perform the requests
  for (Future<network::Response>& f : futures) {
     network::Response const& r = f.get();

     if (r.fail()) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("Communication error locking transactions on ")
                  + r.destination);
    }
    TRI_ASSERT(r.response != nullptr);

    if (r.statusCode() != 202) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
          std::string("lock was denied from ") + r.destination +
          " when trying to check for lockId for hot backup " + backupId);
    }

    bool hasJobID;
    std::string jobId = r.response->header.metaByKey(StaticStrings::AsyncId, hasJobID);
    if (!hasJobID) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
          std::string("lock was denied from ") + r.destination +
          " when trying to check for lockId for hot backup " + backupId);
    }

    dbserverLockIds[r.serverId()] = jobId;
  }

  return arangodb::Result();
}

arangodb::Result hotbackupWaitForLockDBServersTransactions(
  network::ConnectionPool* pool,
  std::string const& backupId,
  std::unordered_map<std::string, std::string> &dbserverLockIds,
  std::vector<ServerID> &lockedServers, double const& lockWait)
{
  // query all remaining jobs here

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;
  reqOpts.timeout = network::Timeout(lockWait + 5.0);

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbserverLockIds.size());

  VPackBufferUInt8 body; // empty body
  for (auto const& lock : dbserverLockIds) {
    futures.emplace_back(network::sendRequest(pool, "server:" + lock.first,
                                              fuerte::RestVerb::Put, "/_api/job/" + lock.second, body,
                                              reqOpts));
  }

  // Perform the requests
//  cc->performRequests(requests, lockWait + 5.0, Logger::BACKUP, false, false);
  for (Future<network::Response>& f : futures) {
     network::Response const& r = f.get();

    if (r.fail()) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("Communication error locking transactions on ")
                  + r.destination);
    }
    TRI_ASSERT(r.response != nullptr);

    // continue on 204 No Content
    if (r.statusCode() == 204) {
      continue;
    }

    VPackSlice slc = r.slice();

    if (!slc.isObject() || !slc.hasKey(StaticStrings::Error) || !slc.get(StaticStrings::Error).isBoolean()) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("invalid response from ") + r.destination +
                  " when trying to freeze transactions for hot backup " +
                  backupId + ": " + slc.toJson());
    }

    if (slc.get(StaticStrings::Error).getBoolean()) {
      LOG_TOPIC("d7a8a", DEBUG, Logger::BACKUP)
          << "failed to acquire lock from " << r.destination << ": " << slc.toJson();
      auto errorNum = slc.get(StaticStrings::ErrorNum).getNumber<int>();
      if (errorNum == TRI_ERROR_LOCK_TIMEOUT) {
        return arangodb::Result(errorNum, slc.get(StaticStrings::ErrorMessage).copyString());
      }
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
          std::string("lock was denied from ") + r.destination +
          " when trying to check for lockId for hot backup " + backupId +
          ": " + slc.toJson());
    }

    if (!slc.hasKey(lockPath) || !slc.get(lockPath).isNumber() ||
        !slc.hasKey("result") || !slc.get("result").isObject()) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("invalid response from ") + r.destination +
                  " when trying to check for lockId for hot backup " +
                  backupId + ": " + slc.toJson());
    }

    uint64_t lockId = 0;
    try {
      lockId = slc.get(lockPath).getNumber<uint64_t>();
      LOG_TOPIC("144f5", DEBUG, Logger::BACKUP)
          << "acquired lock from " << r.destination << " for backupId "
          << backupId << " with lockId " << lockId;
    } catch (std::exception const& e) {
      return arangodb::Result(TRI_ERROR_LOCAL_LOCK_FAILED,
                  std::string("invalid response from ") + r.destination +
                  " when trying to get lockId for hot backup " + backupId +
                  ": " + slc.toJson() + ", msg: " + e.what());
    }

    lockedServers.push_back(r.serverId());
    dbserverLockIds.erase(r.serverId());
  }

  return arangodb::Result();
}

void hotbackupCancelAsyncLocks(
  network::ConnectionPool* pool,
  std::unordered_map<std::string, std::string> &dbserverLockIds,
  std::vector<ServerID> &lockedServers)
{
  // abort all the jobs
  // if a job can not be aborted, assume it has started and add the server to
  // the unlock list

  // cancel all remaining jobs here

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = true;
  reqOpts.timeout = network::Timeout(5.0);

  std::vector<Future<network::Response>> futures;
  futures.reserve(dbserverLockIds.size());

  VPackBufferUInt8 body; // empty body
  for (auto const& lock : dbserverLockIds) {
    futures.emplace_back(network::sendRequest(pool, "server:" + lock.first,
                                              fuerte::RestVerb::Put, "/_api/job/" + lock.second + "/cancel", body,
                                              reqOpts));
  }
}

arangodb::Result hotBackupCoordinator(ClusterFeature& feature, VPackSlice const payload,
                                      VPackBuilder& report) {
  // ToDo: mode
  // HotBackupMode const mode = CONSISTENT;

  /*
    Suggestion for procedure for cluster hotbackup:
    1. Check that ToDo and Pending are empty, if not, delay, back to 1.
       Timeout for giving up.
    2. Stop Supervision, remember if it was on or not.
    3. Check that ToDo and Pending are empty, if not, start Supervision again,
       back to 1.
       4. Get Plan, will have no resigned leaders.
    5. Stop Transactions, if this does not work in time, restore Supervision
       and give up.
    6. Take hotbackups everywhere, if any fails, all failed.
    7. Resume Transactions.
    8. Resume Supervision if it was on.
    9. Keep Maintenance on dbservers on all the time.
  */

  try {
    if (!payload.isNone() &&
        (!payload.isObject() ||
         (payload.hasKey("label") && !payload.get("label").isString()) ||
         (payload.hasKey("timeout") && !payload.get("timeout").isNumber()) ||
         (payload.hasKey("allowInconsistent") &&
          !payload.get("allowInconsistent").isBoolean()) ||
         (payload.hasKey("force") &&
          !payload.get("force").isBoolean()))) {
      events::CreateHotbackup("", TRI_ERROR_BAD_PARAMETER);
      return arangodb::Result(TRI_ERROR_BAD_PARAMETER, BAD_PARAMS_CREATE);
    }

    bool allowInconsistent =
        payload.isNone() ? false : payload.get("allowInconsistent").isTrue();
    bool force = payload.isNone() ? false : payload.get("force").isTrue();

    std::string const backupId = (payload.isObject() && payload.hasKey("label"))
                                     ? payload.get("label").copyString()
                                     : to_string(boost::uuids::random_generator()());
    std::string timeStamp = timepointToString(std::chrono::system_clock::now());

    double timeout = (payload.isObject() && payload.hasKey("timeout"))
                         ? payload.get("timeout").getNumber<double>()
                         : 120.;
    // unreasonably short even under allowInconsistent
    if (timeout < 2.5) {
      auto const tmp = timeout;
      timeout = 2.5;
      LOG_TOPIC("67ae2", WARN, Logger::BACKUP)
        << "Backup timeout " << tmp << " is too short - raising to " << timeout;
    }

    using namespace std::chrono;
    auto end = steady_clock::now() + milliseconds(static_cast<uint64_t>(1000 * timeout));
    ClusterInfo& ci = feature.clusterInfo();

    auto const& nf = feature.server().getFeature<NetworkFeature>();
    network::ConnectionPool* pool = nf.pool();
    if (!pool) {
      // nullptr happens only during controlled shutdown
      events::CreateHotbackup(timeStamp + "_" + backupId, TRI_ERROR_SHUTTING_DOWN);
      return Result(TRI_ERROR_SHUTTING_DOWN, "server is shutting down");
    }

    // Go to backup mode for *timeout* if and only if not already in
    // backup mode. Otherwise we cannot know, why backup mode was activated
    // We specifically want to make sure that no other backup is going on.
    bool supervisionOff = false;
    auto result = ci.agencyHotBackupLock(backupId, timeout, supervisionOff);

    if (!result.ok()) {
      // Failed to go to backup mode
      result.reset(TRI_ERROR_HOT_BACKUP_INTERNAL,
                   std::string("agency lock operation resulted in ") + result.errorMessage());
      LOG_TOPIC("6c73d", ERR, Logger::BACKUP) << result.errorMessage();
      events::CreateHotbackup(timeStamp + "_" + backupId, TRI_ERROR_HOT_BACKUP_INTERNAL);
      return result;
    }

    auto releaseAgencyLock = scopeGuard([&]{
      LOG_TOPIC("52416", DEBUG, Logger::BACKUP)
          << "Releasing agency lock with scope guard! backupId: " << backupId;
      ci.agencyHotBackupUnlock(backupId, timeout, supervisionOff);
    });

    if (end < steady_clock::now()) {
      LOG_TOPIC("352d6", INFO, Logger::BACKUP)
          << "hot backup didn't get to locking phase within " << timeout << "s.";
      // release the lock
      releaseAgencyLock.fire();

      events::CreateHotbackup(timeStamp + "_" + backupId, TRI_ERROR_CLUSTER_TIMEOUT);
      return arangodb::Result(TRI_ERROR_CLUSTER_TIMEOUT,
                              "hot backup timeout before locking phase");
    }

    // acquire agency dump
    auto agency = std::make_shared<VPackBuilder>();
    result = ci.agencyPlan(agency);

    if (!result.ok()) {
      // release the lock
      releaseAgencyLock.fire();
      result.reset(TRI_ERROR_HOT_BACKUP_INTERNAL,
                   std::string("failed to acquire agency dump: ") + result.errorMessage());
      LOG_TOPIC("c014d", ERR, Logger::BACKUP) << result.errorMessage();
      events::CreateHotbackup(timeStamp + "_" + backupId, TRI_ERROR_HOT_BACKUP_INTERNAL);
      return result;
    }

    // Call lock on all database servers

    std::vector<ServerID> dbServers = ci.getCurrentDBServers();
    std::vector<ServerID> lockedServers;
    // We try to hold all write transactions on all dbservers at the same time.
    // The default timeout to get to this state is 120s. We first try for a
    // certain time t, and if not everybody has stopped all transactions within
    // t seconds, we release all locks and try again with t doubled, until the
    // total timeout has been reached. We start with t=15, which gives us
    // 15, 30 and 60 to try before the default timeout of 120s has been reached.
    double lockWait(15.0);
    while (steady_clock::now() < end && !feature.server().isStopping()) {
      result = lockDBServerTransactions(pool, backupId, dbServers, lockWait, lockedServers);
      if (!result.ok()) {
        unlockDBServerTransactions(pool, backupId, lockedServers);
        lockedServers.clear();
        if (result.is(TRI_ERROR_LOCAL_LOCK_FAILED)) {  // Unrecoverable
          // release the lock
          releaseAgencyLock.fire();
          events::CreateHotbackup(timeStamp + "_" + backupId, TRI_ERROR_LOCAL_LOCK_FAILED);
          return result;
        }
      } else {
        break;
      }
      if (lockWait < 3600.0) {
        lockWait *= 2.0;
      }
      std::this_thread::sleep_for(milliseconds(300));
    }

    if (!result.ok() && force) {

      // About this code:
      // it first creates async requests to lock all dbservers.
      //    the corresponding lock ids are stored int the map lockJobIds.
      // Then we continously abort all trx while checking all the above jobs
      //    for completion.
      // If a job was completed then its id is removed from lockJobIds
      //  and the server is added to the lockedServers list.
      // Once lockJobIds is empty or an error occured we exit the loop
      //  and continue on the normal path (as if all servers would have been locked or error-exit)

      // dbserver -> jobId
      std::unordered_map<std::string, std::string> lockJobIds;

      auto releaseLocks = scopeGuard([&]{
        hotbackupCancelAsyncLocks(pool, lockJobIds, lockedServers);
        unlockDBServerTransactions(pool, backupId, lockedServers);
      });

      // we have to reset the timeout, otherwise the code below will exit too soon
      end = steady_clock::now() + milliseconds(static_cast<uint64_t>(1000 * timeout));

      // send the locks
      result = hotbackupAsyncLockDBServersTransactions(pool, backupId, dbServers, lockWait, lockJobIds);
      if (result.fail()) {
        events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
        return result;
      }

      transaction::Manager* mgr = transaction::ManagerFeature::manager();

      while (!lockJobIds.empty()) {
        if (steady_clock::now() > end) {
          return arangodb::Result(TRI_ERROR_CLUSTER_TIMEOUT,
                                  "hot backup timeout before locking phase");
        }

        // kill all transactions
        result = mgr->abortAllManagedWriteTrx(ExecContext::current().user(), true);
        if (result.fail()) {
          events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
          return result;
        }

        // wait for locks, servers that got the lock are removed from lockJobIds
        result = hotbackupWaitForLockDBServersTransactions(pool, backupId, lockJobIds, lockedServers, lockWait);
        if (result.fail()) {
          events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
          return result;
        }

        std::this_thread::sleep_for(milliseconds(300));
      }

      releaseLocks.cancel();
    }

    bool gotLocks = result.ok();

    // In the case we left the above loop with a negative result,
    // and we are in the case of a force backup we want to continue here
    if (!gotLocks && !allowInconsistent) {
      unlockDBServerTransactions(pool, backupId, dbServers);
      // release the lock
      releaseAgencyLock.fire();
      result.reset(
          TRI_ERROR_HOT_BACKUP_INTERNAL,
          std::string(
              "failed to acquire global transaction lock on all db servers: ") +
              result.errorMessage());
      LOG_TOPIC("b7d09", ERR, Logger::BACKUP) << result.errorMessage();
      events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
      return result;
    }

    BackupMeta meta(backupId, "", timeStamp, std::vector<std::string>{},
                    0, 0, static_cast<unsigned int>(dbServers.size()), "", !gotLocks);   // Temporary
    std::vector<std::string> dummy;
    result = hotBackupDBServers(pool, backupId, timeStamp, dbServers, agency->slice(),
                                /* force */ !gotLocks, meta);
    if (!result.ok()) {
      unlockDBServerTransactions(pool, backupId, dbServers);
      // release the lock
      releaseAgencyLock.fire();
      result.reset(TRI_ERROR_HOT_BACKUP_INTERNAL,
                   std::string("failed to hot backup on all db servers: ") +
                       result.errorMessage());
      LOG_TOPIC("6b333", ERR, Logger::BACKUP) << result.errorMessage();
      removeLocalBackups(pool, backupId, dbServers, dummy);
      events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
      return result;
    }

    unlockDBServerTransactions(pool, backupId, dbServers);
    // release the lock
    releaseAgencyLock.fire();

    auto agencyCheck = std::make_shared<VPackBuilder>();
    result = ci.agencyPlan(agencyCheck);
    if (!result.ok()) {
      if (!allowInconsistent) {
        removeLocalBackups(pool, backupId, dbServers, dummy);
      }
      result.reset(TRI_ERROR_HOT_BACKUP_INTERNAL,
                   std::string("failed to acquire agency dump post backup: ") +
                       result.errorMessage() + " backup's integrity is not guaranteed");
      LOG_TOPIC("d4229", ERR, Logger::BACKUP) << result.errorMessage();
      events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
      return result;
    }

    try {
      if (!Helper::equal(agency->slice()[0].get(versionPath),
                         agencyCheck->slice()[0].get(versionPath), false)) {
        if (!allowInconsistent) {
          removeLocalBackups(pool, backupId, dbServers, dummy);
        }
        result.reset(TRI_ERROR_HOT_BACKUP_INTERNAL,
                     "data definition of cluster was changed during hot "
                     "backup: backup's integrity is not guaranteed");
        LOG_TOPIC("0ad21", ERR, Logger::BACKUP) << result.errorMessage();
        events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
        return result;
      }
    } catch (std::exception const& e) {
      removeLocalBackups(pool, backupId, dbServers, dummy);
      result.reset(TRI_ERROR_HOT_BACKUP_INTERNAL,
                   std::string("invalid agency state: ") + e.what());
      LOG_TOPIC("037eb", ERR, Logger::BACKUP) << result.errorMessage();
      events::CreateHotbackup(timeStamp + "_" + backupId, result.errorNumber());
      return result;
    }

    std::replace(timeStamp.begin(), timeStamp.end(), ':', '.');
    {
      VPackObjectBuilder o(&report);
      report.add("id", VPackValue(timeStamp + "_" + backupId));
      report.add("sizeInBytes", VPackValue(meta._sizeInBytes));
      report.add("nrFiles", VPackValue(meta._nrFiles));
      report.add("nrDBServers", VPackValue(meta._nrDBServers));
      report.add("datetime", VPackValue(meta._datetime));
      if (!gotLocks) {
        report.add("potentiallyInconsistent", VPackValue(true));
      }
    }

    events::CreateHotbackup(timeStamp + "_" + backupId, TRI_ERROR_NO_ERROR);
    return arangodb::Result();

  } catch (std::exception const& e) {
    events::CreateHotbackup("", TRI_ERROR_HOT_BACKUP_INTERNAL);
    return arangodb::Result(
        TRI_ERROR_HOT_BACKUP_INTERNAL,
        std::string("caught exception creating cluster backup: ") + e.what());
  }
}

arangodb::Result listHotBackupsOnCoordinator(ClusterFeature& feature, VPackSlice const payload,
                                             VPackBuilder& report) {

  auto const& nf = feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (!pool) {
    // shutdown, leave here
    return TRI_ERROR_SHUTTING_DOWN;
  }

  ClusterInfo& ci = feature.clusterInfo();
  std::vector<ServerID> dbServers = ci.getCurrentDBServers();

  std::unordered_map<std::string, BackupMeta> list;

  VPackSlice idSlice;
  if (payload.isObject() && payload.hasKey("id")) {
    idSlice = payload.get("id");
    if (idSlice.isArray()) {
      for (auto const i : VPackArrayIterator(payload.get("id"))) {
        if (!i.isString()) {
          return arangodb::Result(
              TRI_ERROR_HTTP_BAD_PARAMETER,
              "invalid list JSON: all ids must be string.");
        }
      }
    } else if (!idSlice.isString()) {
      return arangodb::Result(
          TRI_ERROR_HTTP_BAD_PARAMETER,
          "invalid JSON: id must be string or array of strings.");
    }
  } else if (!payload.isNone()) {
    return arangodb::Result(
        TRI_ERROR_HTTP_BAD_PARAMETER,
        "invalid JSON: body must be empty or object with attribute 'id'.");
  } // allow continuation with None slice

  VPackBuilder dummy;

  // Try to get complete listing for 2 minutes
  using namespace std::chrono;
  auto timeout = steady_clock::now() + duration<double>(120.0);
  arangodb::Result result;
  std::chrono::duration<double> wait(1.0);
  while (true) {
    if (feature.server().isStopping()) {
      return Result(TRI_ERROR_SHUTTING_DOWN, "server is shutting down");
    }

    result = hotBackupList(pool, dbServers, idSlice, list, dummy);

    if (!result.ok()) {

      if (payload.isObject() && !idSlice.isNone() && result.is(TRI_ERROR_HTTP_NOT_FOUND)) {
        auto error = std::string("failed to locate backup '") + idSlice.toJson() + "'";
        LOG_TOPIC("2020b", DEBUG, Logger::BACKUP) << error;
        return arangodb::Result(TRI_ERROR_HTTP_NOT_FOUND, error);
      }
      if (steady_clock::now() > timeout) {
        return arangodb::Result(
          TRI_ERROR_CLUSTER_TIMEOUT, "timeout waiting for all db servers to report backup list");
      } else {
        LOG_TOPIC("76865", DEBUG, Logger::BACKUP) << "failed to get a hot backup listing from all db servers waiting " << wait.count() << " seconds";
        std::this_thread::sleep_for(wait);
        wait *= 1.1;
      }
    } else {
      break;
    }
  }

  // Build report
  {
    VPackObjectBuilder o(&report);
    report.add(VPackValue("list"));
    {
      VPackObjectBuilder a(&report);
      for (auto const& i : list) {
        report.add(VPackValue(i.first));
        i.second.toVelocyPack(report);
      }
    }
  }

  return arangodb::Result();
}

arangodb::Result deleteHotBackupsOnCoordinator(ClusterFeature& feature,
                                               VPackSlice const payload,
                                               VPackBuilder& report) {
  std::vector<std::string> deleted;
  VPackBuilder dummy;
  arangodb::Result result;

  auto const& nf = feature.server().getFeature<NetworkFeature>();
  network::ConnectionPool* pool = nf.pool();
  if (!pool) {  // shutdown, leave here
    events::DeleteHotbackup("", TRI_ERROR_SHUTTING_DOWN);
    return TRI_ERROR_SHUTTING_DOWN;
  }

  ClusterInfo& ci = feature.clusterInfo();
  std::vector<ServerID> dbServers = ci.getCurrentDBServers();

  if (!payload.isObject() || !payload.hasKey("id") || !payload.get("id").isString()) {
    events::DeleteHotbackup("", TRI_ERROR_HTTP_BAD_PARAMETER);
    return arangodb::Result(TRI_ERROR_HTTP_BAD_PARAMETER,
                            "Expecting object with key `id` set to backup id.");
  }

  std::string id = payload.get("id").copyString();

  result = removeLocalBackups(pool, id, dbServers, deleted);
  if (!result.ok()) {
    events::DeleteHotbackup(id, result.errorNumber());
    return result;
  }

  {
    VPackObjectBuilder o(&report);
    report.add(VPackValue("id"));
    {
      VPackArrayBuilder a(&report);
      for (auto const& i : deleted) {
        report.add(VPackValue(i));
      }
    }
  }

  result.reset();
  events::DeleteHotbackup(id, TRI_ERROR_NO_ERROR);
  return result;
}

arangodb::Result getEngineStatsFromDBServers(ClusterFeature& feature,
                                             VPackBuilder& report) {
  ClusterInfo& ci = feature.clusterInfo();

  std::vector<ServerID> DBservers = ci.getCurrentDBServers();

  auto* pool = feature.server().getFeature<NetworkFeature>().pool();

  network::RequestOptions reqOpts;
  reqOpts.skipScheduler = false; 
  std::vector<Future<network::Response>> futures;
  futures.reserve(DBservers.size());

  for (std::string const& server : DBservers) {
    futures.emplace_back(
        network::sendRequestRetry(pool, "server:" + server, fuerte::RestVerb::Get,
                                  "/_api/engine/stats", VPackBuffer<uint8_t>(), reqOpts));
  }
  
  auto responses = futures::collectAll(std::move(futures)).get();

  report.openObject();
  for (auto const& tryRes : responses) {
    network::Response const& r = tryRes.get();
      
    if (r.fail()) {
      return {network::fuerteToArangoErrorCode(r), network::fuerteToArangoErrorMessage(r)};
    }

    // cut off "server:" from the destination
    report.add(r.destination.substr(7), r.slice());
  }
  report.close();

  return Result();
}

}  // namespace arangodb
