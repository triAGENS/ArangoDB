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

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ReadLocker.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/Utf8Helper.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterIndexMethods.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Indexes.h"
#include "Indexes/Index.h"
#include "Indexes/IndexFactory.h"
#include "IResearch/IResearchCommon.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/OperationOrigin.h"
#include "Transaction/StandaloneContext.h"
#ifdef USE_V8
#include "Transaction/V8Context.h"
#endif
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utilities/NameValidator.h"
#ifdef USE_V8
#include "V8Server/v8-collection.h"
#endif
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/vocbase.h"

#include <absl/strings/str_cat.h>
#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>

#include <string>
#include <string_view>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::methods;

namespace {
constexpr std::string_view moduleName("index administration");

/// @brief checks if argument is an index name
Result extractIndexName(velocypack::Slice arg, bool extendedNames,
                        std::string& collectionName, std::string& name) {
  TRI_ASSERT(collectionName.empty());
  TRI_ASSERT(name.empty());

  if (!arg.isString()) {
    return {TRI_ERROR_ARANGO_INDEX_HANDLE_BAD};
  }

  std::string_view handle = arg.stringView();
  if (Index::validateHandleName(extendedNames, handle)) {
    std::size_t split = handle.find('/');
    TRI_ASSERT(split != std::string::npos);
    collectionName = std::string(handle.data(), split);
    name = std::string(handle.data() + split + 1, handle.size() - split - 1);
    return {};
  }

  auto res = IndexNameValidator::validateName(extendedNames, handle);
  if (res.ok()) {
    name = std::string(handle.data(), handle.size());
  }

  return res;
}

/// @brief checks if argument is an index identifier
Result extractIndexHandle(velocypack::Slice arg, bool extendedNames,
                          std::string& collectionName, IndexId& iid) {
  TRI_ASSERT(collectionName.empty());
  TRI_ASSERT(iid.empty());

  if (arg.isNumber()) {
    // numeric index id
    iid = static_cast<IndexId>(arg.getUInt());
    return {};
  }

  if (!arg.isString()) {
    return {TRI_ERROR_ARANGO_INDEX_HANDLE_BAD};
  }

  std::string_view handle = arg.stringView();
  if (arangodb::Index::validateHandle(extendedNames, handle)) {
    std::size_t split = handle.find('/');
    TRI_ASSERT(split != std::string::npos);
    collectionName = std::string(handle.data(), split);
    iid = IndexId{StringUtils::uint64(handle.data() + split + 1,
                                      handle.size() - split - 1)};
    return {};
  }

  if (!handle.empty() &&
      !Index::validateId(std::string_view(handle.data(), handle.size()))) {
    return {TRI_ERROR_ARANGO_INDEX_HANDLE_BAD};
  }

  iid = IndexId{StringUtils::uint64(handle.data(), handle.size())};
  return {};
}

}  // namespace

futures::Future<arangodb::Result> Indexes::getIndex(
    LogicalCollection const& collection, VPackSlice indexId, VPackBuilder& out,
    transaction::Methods* trx) {
  // do some magic to parse the iid
  std::string
      id;  // will (eventually) be fully-qualified; "collection/identifier"
  std::string name;  // will be just name or id (no "collection/")
  bool hasName = true;
  VPackSlice idSlice = indexId;
  if (idSlice.isObject() && idSlice.hasKey(StaticStrings::IndexId)) {
    idSlice = idSlice.get(StaticStrings::IndexId);
    hasName = false;
  }
  if (idSlice.isString()) {
    if (idSlice.stringView().find('/') != std::string_view::npos) {
      id = idSlice.copyString();
      name = id.substr(id.find('/') + 1);
    } else {
      name = idSlice.copyString();
      id = collection.name() + "/" + name;
    }
    uint64_t idValue = 0;
    // if name is just a number (numeric id), we wont validate it
    hasName = !absl::SimpleAtoi(name, &idValue);
  } else if (idSlice.isInteger()) {
    name = StringUtils::itoa(idSlice.getUInt());
    id = collection.name() + "/" + name;
    hasName = false;
  } else {
    co_return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
  }

  if (hasName && !name.empty()) {
    bool extendedNames = collection.vocbase().extendedNames();
    if (auto res = IndexNameValidator::validateName(extendedNames, name);
        res.fail()) {
      co_return res;
    }
  }

  VPackBuilder tmp;
  Result res = co_await Indexes::getAll(
      collection, Index::makeFlags(Index::Serialize::Estimates),
      /*withHidden*/ true, tmp, trx);
  if (res.ok()) {
    for (VPackSlice index : VPackArrayIterator(tmp.slice())) {
      if ((index.hasKey(StaticStrings::IndexId) &&
           index.get(StaticStrings::IndexId).compareString(id) == 0) ||
          (hasName && index.hasKey(StaticStrings::IndexName) &&
           index.get(StaticStrings::IndexName).compareString(name) == 0)) {
        out.add(index);
        co_return Result();
      }
    }
  }
  co_return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
}

/// @brief get all indexes, skips view links
futures::Future<arangodb::Result> Indexes::getAll(
    LogicalCollection const& collection,
    std::underlying_type<Index::Serialize>::type flags, bool withHidden,
    VPackBuilder& result, transaction::Methods* inputTrx) {
  VPackBuilder tmp;
  if (ServerState::instance()->isCoordinator()) {
    auto& databaseName = collection.vocbase().name();
    std::string const& cid = collection.name();

    IndexEstMap estimates;

    if (Index::hasFlag(flags, Index::Serialize::Estimates)) {
      auto& feature =
          collection.vocbase().server().getFeature<ClusterFeature>();
      Result rv = selectivityEstimatesOnCoordinator(feature, databaseName, cid,
                                                    estimates);
      if (rv.fail()) {
        co_return Result(
            rv.errorNumber(),
            basics::StringUtils::concatT("could not retrieve estimates: '",
                                         rv.errorMessage(), "'"));
      }

      // we will merge in the index estimates later
      flags &= ~Index::makeFlags(Index::Serialize::Estimates);
    }

    VPackBuilder tmpInner;
    auto& ci = collection.vocbase()
                   .server()
                   .getFeature<ClusterFeature>()
                   .clusterInfo();
    auto c = ci.getCollection(databaseName, cid);
    c->getPhysical()->getIndexesVPack(
        tmpInner, [withHidden, flags](arangodb::Index const* idx,
                                      decltype(flags)& indexFlags) {
          if (withHidden || !idx->isHidden()) {
            indexFlags = flags;
            return true;
          }
          return false;
        });

    tmp.openArray();
    for (VPackSlice s : VPackArrayIterator(tmpInner.slice())) {
      std::string_view id = s.get(StaticStrings::IndexId).stringView();
      auto found = std::find_if(estimates.begin(), estimates.end(),
                                [&id](std::pair<std::string, double> const& v) {
                                  return id == v.first;
                                });
      if (found == estimates.end()) {
        tmp.add(s);  // just copy
      } else {
        tmp.openObject();
        tmp.add(VPackObjectIterator(s, true));
        tmp.add("selectivityEstimate", VPackValue(found->second));
        tmp.close();
      }
    }
    tmp.close();
  } else {
    std::shared_ptr<transaction::Methods> trx;
    if (inputTrx) {
      trx = std::shared_ptr<transaction::Methods>(inputTrx,
                                                  [](transaction::Methods*) {});
    } else {
      auto origin = transaction::OperationOriginREST{::moduleName};
      trx = std::make_shared<SingleCollectionTransaction>(
          transaction::StandaloneContext::create(collection.vocbase(), origin),
          collection, AccessMode::Type::READ);

      Result res = co_await trx->beginAsync();
      if (!res.ok()) {
        co_return res;
      }
    }

    // get list of indexes
    auto indexes = collection.getPhysical()->getAllIndexes();
    tmp.openArray(true);
    for (auto const& idx : indexes) {
      if (!withHidden && idx->isHidden()) {
        continue;
      }
      idx->toVelocyPack(tmp, flags);
    }
    tmp.close();

    if (!inputTrx) {
      Result res = trx->finish({});
      if (res.fail()) {
        co_return res;
      }
    }
  }

  bool mergeEdgeIdxs = !ServerState::instance()->isDBServer();
  double selectivity = 0, memory = 0, cacheSize = 0, cacheUsage = 0,
         cacheLifeTimeHitRate = 0, cacheWindowedHitRate = 0;
  bool useCache = false;

  VPackArrayBuilder a(&result);
  for (VPackSlice index : VPackArrayIterator(tmp.slice())) {
    std::string id = collection.name() + TRI_INDEX_HANDLE_SEPARATOR_CHR +
                     index.get(arangodb::StaticStrings::IndexId).copyString();
    VPackBuilder merge;
    merge.openObject(true);
    merge.add(arangodb::StaticStrings::IndexId,
              arangodb::velocypack::Value(id));

    auto type = index.get(arangodb::StaticStrings::IndexType);
    if (mergeEdgeIdxs &&
        Index::type(type.copyString()) == Index::TRI_IDX_TYPE_EDGE_INDEX) {
      VPackSlice fields = index.get(StaticStrings::IndexFields);
      TRI_ASSERT(fields.isArray() && fields.length() <= 2);

      if (fields.length() == 1) {  // merge indexes
        // read out relevant values
        if (VPackSlice val = index.get("selectivityEstimate"); val.isNumber()) {
          selectivity += val.getNumber<double>();
        }

        if (VPackSlice figures = index.get("figures");
            figures.isObject() && !figures.isEmptyObject()) {
          if (VPackSlice val = figures.get("cacheInUse"); val.isBool()) {
            useCache |= val.getBool();
          }

          if (VPackSlice val = figures.get("memory"); val.isNumber()) {
            memory += val.getNumber<double>();
          }

          if (VPackSlice val = figures.get("cacheSize"); val.isNumber()) {
            cacheSize += val.getNumber<double>();
          }

          if (VPackSlice val = figures.get("cacheUsage"); val.isNumber()) {
            cacheUsage += val.getNumber<double>();
          }

          if (VPackSlice val = figures.get("cacheLifeTimeHitRate");
              val.isNumber()) {
            cacheLifeTimeHitRate += val.getNumber<double>();
          }

          if (VPackSlice val = figures.get("cacheWindowedHitRate");
              val.isNumber()) {
            cacheWindowedHitRate += val.getNumber<double>();
          }
        }

        if (fields[0].compareString(StaticStrings::FromString) == 0) {
          // ignore one part of the edge index
          continue;
        }

        if (fields[0].compareString(StaticStrings::ToString) == 0) {
          // fuse the values of the two edge indexes together
          merge.add(StaticStrings::IndexFields,
                    VPackValue(VPackValueType::Array));
          merge.add(VPackValue(StaticStrings::FromString));
          merge.add(VPackValue(StaticStrings::ToString));
          merge.close();

          merge.add("selectivityEstimate", VPackValue(selectivity / 2));
          if (Index::hasFlag(flags, Index::Serialize::Figures)) {
            merge.add("figures", VPackValue(VPackValueType::Object));
            merge.add("memory", VPackValue(memory));
            if (useCache) {
              merge.add("cacheSize", VPackValue(cacheSize));
              merge.add("cacheUsage", VPackValue(cacheUsage));
              merge.add("cacheLifeTimeHitRate",
                        VPackValue(cacheLifeTimeHitRate / 2));
              merge.add("cacheWindowedHitRate",
                        VPackValue(cacheWindowedHitRate / 2));
            }
            merge.close();
          }
        }
      }
    }
    merge.close();
    merge = VPackCollection::merge(index, merge.slice(), true);
    result.add(merge.slice());
  }
  co_return Result();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index, locally
////////////////////////////////////////////////////////////////////////////////

static futures::Future<Result> EnsureIndexLocal(
    arangodb::LogicalCollection& collection, VPackSlice definition, bool create,
    VPackBuilder& output, std::shared_ptr<Indexes::ProgressTracker> progress,
    replication2::replicated_state::document::Replication2Callback
        replicationCb) {
  auto lambda = [&]() -> futures::Future<futures::Unit> {
    bool created = false;
    std::shared_ptr<arangodb::Index> idx;

    if (create) {
      idx = co_await collection.createIndex(
          definition, created, std::move(progress), std::move(replicationCb));
      TRI_ASSERT(idx != nullptr);
    } else {
      idx = collection.lookupIndex(definition);
      if (idx == nullptr) {
        // Index not found
        THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
      }
    }

    TRI_ASSERT(idx != nullptr);
    VPackBuilder tmp;
    idx->toVelocyPack(tmp, Index::makeFlags(Index::Serialize::Estimates));

    std::string iid = StringUtils::itoa(idx->id().id());
    VPackBuilder b;
    b.openObject();
    b.add("isNewlyCreated", VPackValue(created));
    b.add(StaticStrings::IndexId,
          VPackValue(collection.name() + TRI_INDEX_HANDLE_SEPARATOR_CHR + iid));
    b.close();
    output = VPackCollection::merge(tmp.slice(), b.slice(), false);
  };
  co_return co_await asResult(lambda());
}

Result Indexes::ensureIndexCoordinator(
    arangodb::LogicalCollection const& collection, velocypack::Slice indexDef,
    bool create, velocypack::Builder& resultBuilder) {
  auto& cluster = collection.vocbase().server().getFeature<ClusterFeature>();

  return ClusterIndexMethods::ensureIndexCoordinator(  // create index
      collection, indexDef, create, resultBuilder,
      cluster.indexCreationTimeout());
}

namespace {
std::unordered_set<std::string_view> extractRelevantKeysForSharding(
    VPackSlice indexDef) {
  std::unordered_set<std::string_view> indexKeys;

  auto const fieldsIntoSet = [](VPackSlice slice,
                                std::unordered_set<std::string_view>& result) {
    for (auto f : VPackArrayIterator(slice)) {
      TRI_ASSERT(f.isString());  // after index validation
      result.emplace(f.stringView());
    }
  };

  fieldsIntoSet(indexDef.get(arangodb::StaticStrings::IndexFields), indexKeys);
  if (auto prefixFields = indexDef.get(StaticStrings::IndexPrefixFields);
      prefixFields.isArray()) {
    fieldsIntoSet(prefixFields, indexKeys);
  }

  return indexKeys;
}

}  // namespace

futures::Future<arangodb::Result> Indexes::ensureIndex(
    LogicalCollection& collection, VPackSlice input, bool create,
    VPackBuilder& output, std::shared_ptr<ProgressTracker> progress,
    replication2::replicated_state::document::Replication2Callback
        replicationCb) {
  ErrorCode ensureIndexResult = TRI_ERROR_INTERNAL;
  // always log a message at the end of index creation
  auto logResultToAuditLog = scopeGuard([&]() noexcept {
    try {
      events::CreateIndexEnd(collection.vocbase().name(), collection.name(),
                             input, ensureIndexResult);
    } catch (...) {
      // nothing we can do
    }
  });

  // can read indexes with RO on db and collection. Modifications require RW/RW
  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    auth::Level lvl = exec.databaseAuthLevel();
    bool canModify = exec.canUseCollection(collection.name(), auth::Level::RW);
    bool canRead = exec.canUseCollection(collection.name(), auth::Level::RO);
    if ((create && (lvl != auth::Level::RW || !canModify)) ||
        (lvl == auth::Level::NONE || !canRead)) {
      ensureIndexResult = TRI_ERROR_FORBIDDEN;
      co_return Result(TRI_ERROR_FORBIDDEN);
    }
  }

  VPackBuilder normalized;
  StorageEngine& engine = collection.vocbase().engine();
  auto res = engine.indexFactory().enhanceIndexDefinition(
      input, normalized, create, collection.vocbase());

  if (res.fail()) {
    ensureIndexResult = res.errorNumber();
    co_return res;
  }

  VPackSlice indexDef = normalized.slice();
  // for single server or for cluster when the instance is coordinator,
  // indexes cannot be created covering fields that have preceding or trailing
  // ":", because the case of preceding or trailing ":" is treated as a special
  // case for shardKeys, in which the value of the attribute is read until or
  // starting from where the character ":" of the string is reached, and it
  // doesn't happen for index fields. We don't disallow this usage for dbservers
  // because this check must be only done for indexes that will be created, not
  // for indexes that already exist. Example for shardKeys: ["value:"], if the
  // document has an attribute {"value": "123:abc"}, the shard key would cover
  // "123", which is the substring read until we reach a ":"
  if (create && (ServerState::instance()->isSingleServer() ||
                 ServerState::instance()->isCoordinator())) {
    Index::validateFieldsWithSpecialCase(
        indexDef.get(arangodb::StaticStrings::IndexFields));
  }

  if (ServerState::instance()->isCoordinator()) {
    TRI_ASSERT(indexDef.isObject());

    // check if there is an attempt to create a unique index on non-shard keys
    if (create) {
      Index::validateFields(indexDef);
      auto isUnique =
          indexDef.get(arangodb::StaticStrings::IndexUnique).isTrue();

      /* the following combinations of shardKeys and indexKeys are allowed/not
       allowed:

       shardKeys     indexKeys
       a             a            ok
       a             b        not ok
       a             a b          ok
       a b             a      not ok
       a b             b      not ok
       a b           a b          ok
       a b           a b c        ok
       a b c           a b    not ok
       a b c         a b c        ok
       */

      if (isUnique && collection.numberOfShards() > 1) {
        // unique index, now check if fields and shard keys match

        std::vector<std::string> const& shardKeys = collection.shardKeys();
        std::unordered_set<std::string_view> indexKeys =
            extractRelevantKeysForSharding(indexDef);

        // all shard-keys must be covered by the index
        for (auto& it : shardKeys) {
          if (indexKeys.find(it) == indexKeys.end()) {
            ensureIndexResult = TRI_ERROR_CLUSTER_UNSUPPORTED;
            co_return Result(
                TRI_ERROR_CLUSTER_UNSUPPORTED,
                "shard key '" + it + "' must be present in unique index");
          }
        }
      }
    }
  }

  TRI_ASSERT(!indexDef.isNone());
  // log a message for index creation start
  events::CreateIndexStart(collection.vocbase().name(), collection.name(),
                           indexDef);

  TRI_ASSERT(res.ok());

  // ensure an index, coordinator case
  if (ServerState::instance()->isCoordinator()) {
    VPackBuilder tmp;
#ifdef USE_ENTERPRISE
    res = Indexes::ensureIndexCoordinatorEE(collection, indexDef, create, tmp);
#else
    res = Indexes::ensureIndexCoordinator(collection, indexDef, create, tmp);
#endif
    if (res.ok()) {
      if (tmp.slice().isNone()) {
        // did not find a suitable index
        auto code =
            create ? TRI_ERROR_OUT_OF_MEMORY : TRI_ERROR_ARANGO_INDEX_NOT_FOUND;
        res.reset(code);
      } else {
        // flush estimates
        collection.getPhysical()->flushClusterIndexEstimates();

        // the cluster won't set a proper id value
        // we don't need analyzer revision here
        TRI_ASSERT(output.isEmpty());
        output.openObject();
        for (auto value : velocypack::ObjectIterator{tmp.slice()}) {
          TRI_ASSERT(value.key.isString());
          auto key = value.key.stringView();
          if (key == StaticStrings::IndexId) {
            output.add(key,
                       velocypack::Value{absl::StrCat(
                           collection.name(), TRI_INDEX_HANDLE_SEPARATOR_STR,
                           value.value.stringView())});
          } else if (key !=
                     iresearch::StaticStrings::AnalyzerDefinitionsField) {
            output.add(key, value.value);
          }
        }
        output.close();
      }
    }
  } else {
    res = co_await EnsureIndexLocal(collection, indexDef, create, output,
                                    std::move(progress),
                                    std::move(replicationCb));
  }

  ensureIndexResult = res.errorNumber();
  co_return res;
}

futures::Future<arangodb::Result> Indexes::createIndex(
    LogicalCollection& coll, Index::IndexType type,
    std::vector<std::string> const& fields, bool unique, bool sparse,
    bool estimates) {
  VPackBuilder props;

  props.openObject();
  props.add(arangodb::StaticStrings::IndexType,
            arangodb::velocypack::Value(Index::oldtypeName(type)));
  props.add(arangodb::StaticStrings::IndexFields,
            arangodb::velocypack::Value(VPackValueType::Array));

  for (std::string const& field : fields) {
    props.add(VPackValue(field));
  }

  props.close();
  props.add(arangodb::StaticStrings::IndexUnique,
            arangodb::velocypack::Value(unique));
  props.add(arangodb::StaticStrings::IndexSparse,
            arangodb::velocypack::Value(sparse));
  props.add(arangodb::StaticStrings::IndexEstimates,
            arangodb::velocypack::Value(estimates));
  props.close();

  VPackBuilder ignored;
  co_return co_await ensureIndex(coll, props.slice(), true, ignored);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an index identifier
////////////////////////////////////////////////////////////////////////////////

Result Indexes::extractHandle(arangodb::LogicalCollection const& collection,
                              arangodb::CollectionNameResolver const* resolver,
                              VPackSlice const& val, IndexId& iid,
                              std::string& name) {
  // reset the collection identifier
  std::string collectionName;

  bool extendedNames = collection.vocbase().extendedNames();

  // extract the index identifier from a string
  if (val.isString() || val.isNumber()) {
    // try to extract index handle
    Result res = ::extractIndexHandle(val, extendedNames, collectionName, iid);
    if (!res.ok()) {
      // no index handle found. now try extracting index name
      res = ::extractIndexName(val, extendedNames, collectionName, name);
    }
    if (!res.ok()) {
      return res;
    }
  } else if (val.isObject()) {
    // extract the index identifier from an object
    VPackSlice iidVal = val.get(StaticStrings::IndexId);
    Result res =
        ::extractIndexHandle(iidVal, extendedNames, collectionName, iid);
    if (!res.ok()) {
      VPackSlice nameVal = val.get(StaticStrings::IndexName);
      res = ::extractIndexName(nameVal, extendedNames, collectionName, name);
      if (!res.ok()) {
        return {TRI_ERROR_ARANGO_INDEX_HANDLE_BAD};
      }
    }
  }

  if (!collectionName.empty() &&
      !methods::Collections::hasName(*resolver, collection, collectionName)) {
    // I wish this error provided me with more information!
    // e.g. 'cannot access index outside the collection it was defined in'
    return {TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST};
  }

  return {};
}

auto constexpr getHandle = [](LogicalCollection& collection,
                              velocypack::Slice indexArg,
                              CollectionNameResolver const* resolver,
                              transaction::Methods* trx = nullptr)
    -> futures::Future<ResultT<std::pair<IndexId, std::string>>> {
  IndexId iid = IndexId::none();
  std::string name;
  Result res =
      Indexes::extractHandle(collection, resolver, indexArg, iid, name);

  if (!res.ok()) {
    events::DropIndex(collection.vocbase().name(), collection.name(), "",
                      res.errorNumber());
    co_return res;
  }

  if (iid.empty() && !name.empty()) {
    VPackBuilder builder;
    res =
        co_await methods::Indexes::getIndex(collection, indexArg, builder, trx);
    if (!res.ok()) {
      events::DropIndex(collection.vocbase().name(), collection.name(), "",
                        res.errorNumber());
      co_return res;
    }

    VPackSlice idSlice = builder.slice().get(StaticStrings::IndexId);
    res = Indexes::extractHandle(collection, resolver, idSlice, iid, name);

    if (!res.ok()) {
      events::DropIndex(collection.vocbase().name(), collection.name(), "",
                        res.errorNumber());
    }
  }

  co_return std::pair(iid, std::move(name));
};

futures::Future<arangodb::Result> Indexes::drop(LogicalCollection& collection,
                                                velocypack::Slice indexArg) {
  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    if (exec.databaseAuthLevel() != auth::Level::RW ||
        !exec.canUseCollection(collection.name(), auth::Level::RW)) {
      events::DropIndex(collection.vocbase().name(), collection.name(), "",
                        TRI_ERROR_FORBIDDEN);
      co_return {TRI_ERROR_FORBIDDEN};
    }
  }

  if (ServerState::instance()->isCoordinator()) {
    CollectionNameResolver resolver(collection.vocbase());
    auto handleRes = co_await getHandle(collection, indexArg, &resolver);
    if (!handleRes.ok()) {
      co_return std::move(handleRes).result();
    }
    auto&& [iid, name] = handleRes.get();
    auto const indexId = iid;

    co_return co_await dropCoordinator(collection, indexId);
  } else {
    co_return co_await dropDBServer(collection, indexArg);
  }
}

futures::Future<arangodb::Result> Indexes::drop(LogicalCollection& collection,
                                                IndexId indexId) {
  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    if (exec.databaseAuthLevel() != auth::Level::RW ||
        !exec.canUseCollection(collection.name(), auth::Level::RW)) {
      events::DropIndex(collection.vocbase().name(), collection.name(), "",
                        TRI_ERROR_FORBIDDEN);
      co_return {TRI_ERROR_FORBIDDEN};
    }
  }

  if (ServerState::instance()->isCoordinator()) {
    co_return co_await dropCoordinator(collection, indexId);
  } else {
    co_return co_await dropDBServer(collection, indexId);
  }
}

futures::Future<arangodb::Result> Indexes::dropCoordinator(
    LogicalCollection& collection, IndexId indexId) {
  // flush estimates
  collection.getPhysical()->flushClusterIndexEstimates();

#ifdef USE_ENTERPRISE
  auto res = Indexes::dropCoordinatorEE(collection, indexId);
#else
  auto res =
      ClusterIndexMethods::dropIndexCoordinator(collection, indexId, 0.0);
#endif
  co_return res;
}

std::unique_ptr<SingleCollectionTransaction> Indexes::createTrxForDrop(
    LogicalCollection& collection) {
  auto origin = transaction::OperationOriginREST{::moduleName};
  transaction::Options trxOpts;
  trxOpts.requiresReplication = false;
#ifdef USE_V8
  return std::make_unique<SingleCollectionTransaction>(
      transaction::V8Context::createWhenRequired(collection.vocbase(), origin,
                                                 false),
      collection, AccessMode::Type::EXCLUSIVE, trxOpts);
#else
  return std::make_unique<SingleCollectionTransaction>(
      transaction::StandaloneContext::create(collection.vocbase(), origin),
      collection, AccessMode::Type::EXCLUSIVE, trxOpts);
#endif
}

template<typename IndexSpec>
requires std::is_same_v<IndexSpec, IndexId> or
    std::is_same_v<IndexSpec, velocypack::Slice>
        futures::Future<arangodb::Result> Indexes::dropDBServer(
            LogicalCollection& col, IndexSpec indexSpec) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  READ_LOCKER(readLocker, col.vocbase()._inventoryLock);

  auto trx = createTrxForDrop(col);

  auto beginRes = co_await trx->beginAsync();
  auto& collection = *trx->documentCollection();

  if (!beginRes.ok()) {
    events::DropIndex(collection.vocbase().name(), collection.name(), "",
                      beginRes.errorNumber());
    co_return beginRes;
  }

  auto indexIdRes = co_await std::invoke(
      overload{
          [](IndexId indexId) -> futures::Future<ResultT<IndexId>> {
            co_return indexId;
          },
          [&](velocypack::Slice indexArg) -> futures::Future<ResultT<IndexId>> {
            auto res = co_await getHandle(collection, indexArg, trx->resolver(),
                                          trx.get());
            if (!res.ok()) {
              co_return std::move(res).result();
            }
            auto&& [iid, name] = res.get();
            co_return iid;
          },
      },
      indexSpec);
  if (!indexIdRes.ok()) {
    co_return std::move(indexIdRes).result();
  }
  auto const indexId = indexIdRes.get();

  std::shared_ptr<Index> idx = collection.lookupIndex(indexId);
  if (!idx || idx->id().empty() || idx->id().isPrimary()) {
    events::DropIndex(collection.vocbase().name(), collection.name(),
                      std::to_string(indexId.id()),
                      TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
    co_return {TRI_ERROR_ARANGO_INDEX_NOT_FOUND};
  }
  if (!idx->canBeDropped()) {
    events::DropIndex(collection.vocbase().name(), collection.name(),
                      std::to_string(indexId.id()), TRI_ERROR_FORBIDDEN);
    co_return {TRI_ERROR_FORBIDDEN};
  }

  auto res = collection.dropIndex(indexId);
  events::DropIndex(collection.vocbase().name(), collection.name(),
                    std::to_string(indexId.id()), res.errorNumber());
  co_return res;
}

template futures::Future<arangodb::Result> Indexes::dropDBServer<IndexId>(
    LogicalCollection&, IndexId);
template futures::Future<arangodb::Result>
Indexes::dropDBServer<velocypack::Slice>(LogicalCollection&, velocypack::Slice);
