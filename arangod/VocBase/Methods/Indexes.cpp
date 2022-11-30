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

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Common.h"
#include "Basics/ReadLocker.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/conversions.h"
#include "Basics/tri-strings.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "Indexes.h"
#include "Indexes/Index.h"
#include "Indexes/IndexFactory.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/Hints.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/V8Context.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utilities/NameValidator.h"
#include "V8Server/v8-collection.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"
#include "VocBase/vocbase.h"
#include "Logger/Logger.h"
#include "Logger/LogMacros.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <regex>

namespace {
using namespace arangodb;
using namespace arangodb::methods;

Result getHandle(LogicalCollection const* collection,
                 velocypack::Slice const indexArg, IndexId& iid,
                 std::string& name, CollectionNameResolver const* resolver,
                 transaction::Methods* trx = nullptr) {
  Result res =
      Indexes::extractHandle(collection, resolver, indexArg, iid, name);

  if (!res.ok()) {
    return res;
  }

  if (iid.empty() && !name.empty()) {
    VPackBuilder builder;
    res = Indexes::getIndex(collection, indexArg, builder, trx);
    if (!res.ok()) {
      return res;
    }

    VPackSlice idSlice = builder.slice().get(StaticStrings::IndexId);
    Result res =
        Indexes::extractHandle(collection, resolver, idSlice, iid, name);
  }
  return res;
};
}

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::methods;

Result Indexes::getIndex(LogicalCollection const* collection,
                         VPackSlice indexId, VPackBuilder& out,
                         transaction::Methods* trx) {
  // do some magic to parse the iid
  std::string
      id;  // will (eventually) be fully-qualified; "collection/identifier"
  std::string name;  // will be just name or id (no "collection/")
  VPackSlice idSlice = indexId;
  if (idSlice.isObject() && idSlice.hasKey(StaticStrings::IndexId)) {
    idSlice = idSlice.get(StaticStrings::IndexId);
  }
  if (idSlice.isString()) {
    std::regex re = std::regex("^([a-zA-Z0-9\\-_]+)\\/([a-zA-Z0-9\\-_]+)$",
                               std::regex::ECMAScript);
    if (std::regex_match(idSlice.copyString(), re)) {
      id = idSlice.copyString();
      name = id.substr(id.find_first_of("/") + 1);
    } else {
      name = idSlice.copyString();
      id = collection->name() + "/" + name;
    }
  } else if (idSlice.isInteger()) {
    name = StringUtils::itoa(idSlice.getUInt());
    id = collection->name() + "/" + name;
  } else {
    return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
  }

  VPackBuilder tmp;
  Result res =
      Indexes::getAll(collection, Index::makeFlags(Index::Serialize::Estimates),
                      /*withHidden*/ true, tmp, trx);
  if (res.ok()) {
    for (VPackSlice index : VPackArrayIterator(tmp.slice())) {
      if ((index.hasKey(StaticStrings::IndexId) &&
           index.get(StaticStrings::IndexId).compareString(id) == 0) ||
          (index.hasKey(StaticStrings::IndexName) &&
           index.get(StaticStrings::IndexName).compareString(name) == 0)) {
        out.add(index);
        return Result();
      }
    }
  }
  return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
}

/// @brief get all indexes, skips view links
arangodb::Result Indexes::getAll(
    LogicalCollection const* collection,
    std::underlying_type<Index::Serialize>::type flags, bool withHidden,
    VPackBuilder& result, transaction::Methods* inputTrx) {
  VPackBuilder tmp;
  if (ServerState::instance()->isCoordinator()) {
    TRI_ASSERT(collection);
    auto& databaseName = collection->vocbase().name();
    std::string const& cid = collection->name();

    IndexEstMap estimates;

    if (Index::hasFlag(flags, Index::Serialize::Estimates)) {
      auto& feature =
          collection->vocbase().server().getFeature<ClusterFeature>();
      Result rv = selectivityEstimatesOnCoordinator(feature, databaseName, cid,
                                                    estimates);
      if (rv.fail()) {
        return Result(rv.errorNumber(), basics::StringUtils::concatT(
                                            "could not retrieve estimates: '",
                                            rv.errorMessage(), "'"));
      }

      // we will merge in the index estimates later
      flags &= ~Index::makeFlags(Index::Serialize::Estimates);
    }

    VPackBuilder tmpInner;
    auto& ci = collection->vocbase()
                   .server()
                   .getFeature<ClusterFeature>()
                   .clusterInfo();
    auto c = ci.getCollection(databaseName, cid);
    c->getIndexesVPack(tmpInner,
                       [withHidden, flags](arangodb::Index const* idx,
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
      trx = std::make_shared<SingleCollectionTransaction>(
          transaction::StandaloneContext::Create(collection->vocbase()),
          *collection, AccessMode::Type::READ);

      // we actually need this hint here, so that the collection is not
      // loaded if it has status unloaded.
      trx->addHint(transaction::Hints::Hint::NO_USAGE_LOCK);

      Result res = trx->begin();
      if (!res.ok()) {
        return res;
      }
    }

    // get list of indexes
    auto indexes = collection->getIndexes();

    tmp.openArray(true);
    for (std::shared_ptr<arangodb::Index> const& idx : indexes) {
      if (!withHidden && idx->isHidden()) {
        continue;
      }
      idx->toVelocyPack(tmp, flags);
    }
    tmp.close();

    if (!inputTrx) {
      Result res = trx->finish({});
      if (res.fail()) {
        return res;
      }
    }
  }

  bool mergeEdgeIdxs = !ServerState::instance()->isDBServer();
  double selectivity = 0, memory = 0, cacheSize = 0, cacheUsage = 0,
         cacheLifeTimeHitRate = 0, cacheWindowedHitRate = 0;
  bool useCache = false;

  VPackArrayBuilder a(&result);
  for (VPackSlice index : VPackArrayIterator(tmp.slice())) {
    std::string id = collection->name() + TRI_INDEX_HANDLE_SEPARATOR_CHR +
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
  return Result();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index, locally
////////////////////////////////////////////////////////////////////////////////

static Result EnsureIndexLocal(arangodb::LogicalCollection* collection,
                               VPackSlice definition, bool create,
                               VPackBuilder& output) {
  TRI_ASSERT(collection != nullptr);

  return arangodb::basics::catchVoidToResult([&]() -> void {
    bool created = false;
    std::shared_ptr<arangodb::Index> idx;

    if (create) {
      idx = collection->createIndex(definition, created);
      TRI_ASSERT(idx != nullptr);
    } else {
      idx = collection->lookupIndex(definition);
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
    b.add(
        StaticStrings::IndexId,
        VPackValue(collection->name() + TRI_INDEX_HANDLE_SEPARATOR_CHR + iid));
    b.close();
    output = VPackCollection::merge(tmp.slice(), b.slice(), false);
  });
}

Result Indexes::ensureIndexCoordinator(
    arangodb::LogicalCollection const* collection, VPackSlice const& indexDef,
    bool create, VPackBuilder& resultBuilder) {
  TRI_ASSERT(collection != nullptr);
  auto& cluster = collection->vocbase().server().getFeature<ClusterFeature>();

  return cluster.clusterInfo().ensureIndexCoordinator(  // create index
      *collection, indexDef, create, resultBuilder,
      cluster.indexCreationTimeout());
}

Result Indexes::ensureIndex(LogicalCollection* collection, VPackSlice input,
                            bool create, VPackBuilder& output) {
  ErrorCode ensureIndexResult = TRI_ERROR_INTERNAL;
  // always log a message at the end of index creation
  auto logResultToAuditLog = scopeGuard([&]() noexcept {
    try {
      events::CreateIndexEnd(collection->vocbase().name(), collection->name(),
                             input, ensureIndexResult);
    } catch (...) {
      // nothing we can do
    }
  });

  // can read indexes with RO on db and collection. Modifications require RW/RW
  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    auth::Level lvl = exec.databaseAuthLevel();
    bool canModify = exec.canUseCollection(collection->name(), auth::Level::RW);
    bool canRead = exec.canUseCollection(collection->name(), auth::Level::RO);
    if ((create && (lvl != auth::Level::RW || !canModify)) ||
        (lvl == auth::Level::NONE || !canRead)) {
      ensureIndexResult = TRI_ERROR_FORBIDDEN;
      return Result(TRI_ERROR_FORBIDDEN);
    }
  }

  TRI_ASSERT(collection);
  VPackBuilder normalized;
  StorageEngine& engine = collection->vocbase()
                              .server()
                              .getFeature<EngineSelectorFeature>()
                              .engine();
  auto res = engine.indexFactory().enhanceIndexDefinition(
      input, normalized, create, collection->vocbase());

  if (res.fail()) {
    ensureIndexResult = res.errorNumber();
    return res;
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
      auto v = indexDef.get(arangodb::StaticStrings::IndexUnique);

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

      if (v.isBoolean() && v.getBoolean()) {
        // unique index, now check if fields and shard keys match
        auto flds = indexDef.get(arangodb::StaticStrings::IndexFields);

        if (flds.isArray() && collection->numberOfShards() > 1) {
          std::vector<std::string> const& shardKeys = collection->shardKeys();
          std::unordered_set<std::string> indexKeys;
          size_t n = static_cast<size_t>(flds.length());

          for (size_t i = 0; i < n; ++i) {
            VPackSlice f = flds.at(i);
            if (!f.isString()) {
              // index attributes must be strings
              ensureIndexResult = TRI_ERROR_INTERNAL;
              return Result(TRI_ERROR_INTERNAL,
                            "index field names should be strings");
            }
            indexKeys.emplace(f.copyString());
          }

          // all shard-keys must be covered by the index
          for (auto& it : shardKeys) {
            if (indexKeys.find(it) == indexKeys.end()) {
              ensureIndexResult = TRI_ERROR_CLUSTER_UNSUPPORTED;
              return Result(
                  TRI_ERROR_CLUSTER_UNSUPPORTED,
                  "shard key '" + it + "' must be present in unique index");
            }
          }
        }
      }
    }
  }

  TRI_ASSERT(!indexDef.isNone());
  // log a message for index creation start
  events::CreateIndexStart(collection->vocbase().name(), collection->name(),
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
        collection->getPhysical()->flushClusterIndexEstimates();

        // the cluster won't set a proper id value
        std::string iid = tmp.slice().get(StaticStrings::IndexId).copyString();
        VPackBuilder b;
        b.openObject();
        b.add(StaticStrings::IndexId,
              VPackValue(collection->name() + TRI_INDEX_HANDLE_SEPARATOR_CHR +
                         iid));
        b.close();
        output = VPackCollection::merge(tmp.slice(), b.slice(), false);
      }
    }
  } else {
    res = EnsureIndexLocal(collection, indexDef, create, output);
  }

  ensureIndexResult = res.errorNumber();
  return res;
}

arangodb::Result Indexes::createIndex(LogicalCollection* coll,
                                      Index::IndexType type,
                                      std::vector<std::string> const& fields,
                                      bool unique, bool sparse,
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
  return ensureIndex(coll, props.slice(), true, ignored);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if argument is an index identifier
////////////////////////////////////////////////////////////////////////////////

static bool ExtractIndexHandle(VPackSlice const& arg, bool extendedNames,
                               std::string& collectionName, IndexId& iid) {
  TRI_ASSERT(collectionName.empty());
  TRI_ASSERT(iid.empty());

  if (arg.isNumber()) {
    // numeric index id
    iid = (IndexId)arg.getUInt();
    return true;
  }

  if (!arg.isString()) {
    return false;
  }

  std::string_view handle = arg.stringView();
  if (arangodb::Index::validateHandle(extendedNames, handle)) {
    std::size_t split = handle.find('/');
    TRI_ASSERT(split != std::string::npos);
    collectionName = std::string(handle.data(), split);
    iid = IndexId{StringUtils::uint64(handle.data() + split + 1,
                                      handle.size() - split - 1)};
    return true;
  }

  if (!handle.empty() &&
      !Index::validateId(std::string_view(handle.data(), handle.size()))) {
    return false;
  }
  iid = IndexId{StringUtils::uint64(handle.data(), handle.size())};
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if argument is an index name
////////////////////////////////////////////////////////////////////////////////

static bool ExtractIndexName(VPackSlice const& arg, bool extendedNames,
                             std::string& collectionName, std::string& name) {
  TRI_ASSERT(collectionName.empty());
  TRI_ASSERT(name.empty());

  if (!arg.isString()) {
    return false;
  }

  std::string_view handle = arg.stringView();
  if (arangodb::Index::validateHandleName(extendedNames, handle)) {
    std::size_t split = handle.find('/');
    TRI_ASSERT(split != std::string::npos);
    collectionName = std::string(handle.data(), split);
    name = std::string(handle.data() + split + 1, handle.size() - split - 1);
    return true;
  }

  if (IndexNameValidator::isAllowedName(extendedNames, handle)) {
    name = std::string(handle.data(), handle.size());
    return true;
  }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an index identifier
////////////////////////////////////////////////////////////////////////////////

Result Indexes::extractHandle(arangodb::LogicalCollection const* collection,
                              arangodb::CollectionNameResolver const* resolver,
                              VPackSlice const& val, IndexId& iid,
                              std::string& name) {
  // reset the collection identifier
  std::string collectionName;

  // assume we are already loaded
  TRI_ASSERT(collection != nullptr);

  bool extendedNames = collection->vocbase()
                           .server()
                           .getFeature<DatabaseFeature>()
                           .extendedNamesForCollections();

  // extract the index identifier from a string
  if (val.isString() || val.isNumber()) {
    if (!ExtractIndexHandle(val, extendedNames, collectionName, iid) &&
        !ExtractIndexName(val, extendedNames, collectionName, name)) {
      return Result(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
    }
  } else if (val.isObject()) {
    // extract the index identifier from an object
    VPackSlice iidVal = val.get(StaticStrings::IndexId);
    if (!ExtractIndexHandle(iidVal, extendedNames, collectionName, iid)) {
      VPackSlice nameVal = val.get(StaticStrings::IndexName);
      if (!ExtractIndexName(nameVal, extendedNames, collectionName, name)) {
        return Result(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
      }
    }
  }

  if (!collectionName.empty()) {
    if (!methods::Collections::hasName(*resolver, *collection,
                                       collectionName)) {
      // I wish this error provided me with more information!
      // e.g. 'cannot access index outside the collection it was defined in'
      return Result(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    }
  }
  return Result();
}

arangodb::Result Indexes::update(LogicalCollection* collection,
  velocypack::Slice const indexArg,
  velocypack::Slice const body,
  velocypack::Builder& output) {
  TRI_ASSERT(collection != nullptr);

  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    if (exec.databaseAuthLevel() != auth::Level::RW ||
        !exec.canUseCollection(collection->name(), auth::Level::RW)) {
      // FIXME: Do we need an event here?
      //events::UpdateIndex(collection->vocbase().name(), collection->name(), "",
      //                  TRI_ERROR_FORBIDDEN);
      return TRI_ERROR_FORBIDDEN;
    }
  }
  IndexId iid = IndexId::none();
  std::string name;
  if (ServerState::instance()->isCoordinator()) {
    CollectionNameResolver resolver(collection->vocbase());
    Result res = getHandle(collection, indexArg, iid, name, &resolver);
    if (!res.ok()) {
      // FIXME: Do we need an event here?
      //events::UpdateIndex(collection->vocbase().name(), collection->name(), "",
      //                  res.errorNumber());
      return res;
    }

    auto& ci = collection->vocbase()
                   .server()
                   .getFeature<ClusterFeature>()
                   .clusterInfo();
    // FIXME: implement cluster update
   // res = ci.updateIndexCoordinator(
   //     collection->vocbase().name(), std::to_string(collection->id().id()),
   //     body);
    return res;
  } else {
    READ_LOCKER(readLocker, collection->vocbase()._inventoryLock);

    SingleCollectionTransaction trx(transaction::V8Context::CreateWhenRequired(
                                        collection->vocbase(), false),
                                    *collection, AccessMode::Type::EXCLUSIVE);
    Result res = trx.begin();

    if (!res.ok()) {
      // FIXME: Do we need an event here?
      //events::UpdateIndex(collection->vocbase().name(), collection->name(), "",
      //                  res.errorNumber());
      return res;
    }

    LogicalCollection* col = trx.documentCollection();
    res = getHandle(collection, indexArg, iid, name, trx.resolver(), &trx);
    if (!res.ok()) {
      // FIXME: Do we need an event here?
      //events::UpdateIndex(collection->vocbase().name(), collection->name(), "",
      //                  res.errorNumber());
      return res;
    }
    res = col->updateIndex(iid, body, output);
    // FIXME: Do we need an event here?
    //events::UpdateIndex(collection->vocbase().name(), collection->name(),
    //                  std::to_string(iid.id()), res.errorNumber());
    return res;
  }
  return {};
}

arangodb::Result Indexes::drop(LogicalCollection* collection,
                               VPackSlice const& indexArg) {
  TRI_ASSERT(collection != nullptr);

  ExecContext const& exec = ExecContext::current();
  if (!exec.isSuperuser()) {
    if (exec.databaseAuthLevel() != auth::Level::RW ||
        !exec.canUseCollection(collection->name(), auth::Level::RW)) {
      events::DropIndex(collection->vocbase().name(), collection->name(), "",
                        TRI_ERROR_FORBIDDEN);
      return TRI_ERROR_FORBIDDEN;
    }
  }
  /*
   VPackBuilder tmp;
    idx->toVelocyPack(tmp, Index::makeFlags(Index::Serialize::Estimates));

    std::string iid = StringUtils::itoa(idx->id().id());
    VPackBuilder b;
    b.openObject();
    b.add("isNewlyCreated", VPackValue(created));
    b.add(
        StaticStrings::IndexId,
        VPackValue(collection->name() + TRI_INDEX_HANDLE_SEPARATOR_CHR + iid));
    b.close();
    output = VPackCollection::merge(tmp.slice(), b.slice(), false);*/

  IndexId iid = IndexId::none();
  std::string name;
  if (ServerState::instance()->isCoordinator()) {
    CollectionNameResolver resolver(collection->vocbase());
    Result res = getHandle(collection, indexArg, iid, name, &resolver);
    if (!res.ok()) {
      events::DropIndex(collection->vocbase().name(), collection->name(), "",
                        res.errorNumber());
      return res;
    }

    // flush estimates
    collection->getPhysical()->flushClusterIndexEstimates();

#ifdef USE_ENTERPRISE
    res = Indexes::dropCoordinatorEE(collection, iid);
#else
    auto& ci = collection->vocbase()
                   .server()
                   .getFeature<ClusterFeature>()
                   .clusterInfo();
    res = ci.dropIndexCoordinator(  // drop index
        collection->vocbase().name(), std::to_string(collection->id().id()),
        iid, 0.0  // args
    );
#endif
    return res;
  } else {
    READ_LOCKER(readLocker, collection->vocbase()._inventoryLock);

    transaction::Options trxOpts;
    trxOpts.requiresReplication = false;
    SingleCollectionTransaction trx(transaction::V8Context::CreateWhenRequired(
                                        collection->vocbase(), false),
                                    *collection, AccessMode::Type::EXCLUSIVE,
                                    trxOpts);
    Result res = trx.begin();

    if (!res.ok()) {
      events::DropIndex(collection->vocbase().name(), collection->name(), "",
                        res.errorNumber());
      return res;
    }

    LogicalCollection* col = trx.documentCollection();
    res = getHandle(collection, indexArg, iid, name, trx.resolver(), &trx);
    if (!res.ok()) {
      events::DropIndex(collection->vocbase().name(), collection->name(), "",
                        res.errorNumber());
      return res;
    }

    std::shared_ptr<Index> idx = collection->lookupIndex(iid);
    if (!idx || idx->id().empty() || idx->id().isPrimary()) {
      events::DropIndex(collection->vocbase().name(), collection->name(),
                        std::to_string(iid.id()),
                        TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
      return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
    }
    if (!idx->canBeDropped()) {
      events::DropIndex(collection->vocbase().name(), collection->name(),
                        std::to_string(iid.id()), TRI_ERROR_FORBIDDEN);
      return Result(TRI_ERROR_FORBIDDEN);
    }

    bool ok = col->dropIndex(idx->id());
    auto code = ok ? TRI_ERROR_NO_ERROR : TRI_ERROR_FAILED;
    events::DropIndex(collection->vocbase().name(), collection->name(),
                      std::to_string(iid.id()), code);
    return Result(code);
  }
}
