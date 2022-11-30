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
/// @author Michael Hackstein
/// @author Daniel H. Larkin
////////////////////////////////////////////////////////////////////////////////

#include "LogicalCollection.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryCache.h"
#include "Basics/Mutex.h"
#include "Basics/ReadLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/ServerState.h"
#include "Logger/LogMacros.h"
#include "Replication/ReplicationFeature.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/StateMachines/Document/DocumentStateMachine.h"
#include "RestServer/DatabaseFeature.h"
#include "Sharding/ShardingInfo.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utilities/NameValidator.h"
#include "VocBase/ComputedValues.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/Validators.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/Sharding/ShardingStrategyEE.h"
#endif

#include <absl/strings/str_cat.h>
#include <fmt/core.h>
#include <fmt/ostream.h>

#include <velocypack/Collection.h>
#include <velocypack/Utf8Helper.h>

using namespace arangodb;
using Helper = basics::VelocyPackHelper;

namespace {

static std::string translateStatus(TRI_vocbase_col_status_e status) {
  switch (status) {
    case TRI_VOC_COL_STATUS_LOADED:
      return "loaded";
    case TRI_VOC_COL_STATUS_DELETED:
      return "deleted";
    case TRI_VOC_COL_STATUS_CORRUPTED:
    default:
      return "unknown";
  }
}

std::string readGloballyUniqueId(velocypack::Slice info) {
  auto guid = basics::VelocyPackHelper::getStringValue(
      info, StaticStrings::DataSourceGuid, StaticStrings::Empty);

  if (!guid.empty()) {
    // check if the globallyUniqueId is only numeric. This causes ambiguities
    // later and can only happen (only) for collections created with v3.3.0 (the
    // GUID generation process was changed in v3.3.1 already to fix this issue).
    // remove the globallyUniqueId so a new one will be generated server.side
    bool validNumber = false;
    NumberUtils::atoi_positive<uint64_t>(guid.data(), guid.data() + guid.size(),
                                         validNumber);
    if (!validNumber) {
      // GUID is not just numeric, this is fine
      return guid;
    }

    // GUID is only numeric - we must not use it
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // this should never happen for any collections created during testing. the
    // only way to make this happen is using a collection created with v3.3.0,
    // which we will not have in our tests.
    TRI_ASSERT(false);
#endif
  }

  auto version = basics::VelocyPackHelper::getNumericValue<uint32_t>(
      info, StaticStrings::Version,
      static_cast<uint32_t>(LogicalCollection::currentVersion()));

  // predictable UUID for legacy collections
  if (static_cast<LogicalCollection::Version>(version) <
          LogicalCollection::Version::v33 &&
      info.isObject()) {
    return basics::VelocyPackHelper::getStringValue(
        info, StaticStrings::DataSourceName, StaticStrings::Empty);
  }

  return StaticStrings::Empty;
}

}  // namespace

// The Slice contains the part of the plan that
// is relevant for this collection.
LogicalCollection::LogicalCollection(TRI_vocbase_t& vocbase, VPackSlice info,
                                     bool isAStub)
    : LogicalDataSource(
          *this, vocbase, DataSourceId{Helper::extractIdValue(info)},
          ::readGloballyUniqueId(info),
          DataSourceId{
              Helper::stringUInt64(info.get(StaticStrings::DataSourcePlanId))},
          Helper::getStringValue(info, StaticStrings::DataSourceName, ""),
          NameValidator::isSystemName(Helper::getStringValue(
              info, StaticStrings::DataSourceName, "")) &&
              Helper::getBooleanValue(info, StaticStrings::DataSourceSystem,
                                      false),
          Helper::getBooleanValue(info, StaticStrings::DataSourceDeleted,
                                  false)),
      _version(static_cast<Version>(Helper::getNumericValue<uint32_t>(
          info, StaticStrings::Version,
          static_cast<uint32_t>(currentVersion())))),
      _v8CacheVersion(0),
      _type(Helper::getNumericValue<TRI_col_type_e, int>(
          info, StaticStrings::DataSourceType, TRI_COL_TYPE_DOCUMENT)),
      _status(Helper::getNumericValue<TRI_vocbase_col_status_e, int>(
          info, "status", TRI_VOC_COL_STATUS_CORRUPTED)),
      _isAStub(isAStub),
#ifdef USE_ENTERPRISE
      _isDisjoint(
          Helper::getBooleanValue(info, StaticStrings::IsDisjoint, false)),
      _isSmart(Helper::getBooleanValue(info, StaticStrings::IsSmart, false)),
      _isSmartChild(
          Helper::getBooleanValue(info, StaticStrings::IsSmartChild, false)),
#endif
      _allowUserKeys(
          Helper::getBooleanValue(info, StaticStrings::AllowUserKeys, true)),
      _waitForSync(Helper::getBooleanValue(
          info, StaticStrings::WaitForSyncString, false)),
      _usesRevisionsAsDocumentIds(Helper::getBooleanValue(
          info, StaticStrings::UsesRevisionsAsDocumentIds, false)),
      _syncByRevision(determineSyncByRevision()),
      _countCache(/*ttl*/ system() ? 900.0 : 180.0),
      _physical(vocbase.server()
                    .getFeature<EngineSelectorFeature>()
                    .engine()
                    .createPhysicalCollection(*this, info)) {

  TRI_IF_FAILURE("disableRevisionsAsDocumentIds") {
    _usesRevisionsAsDocumentIds.store(false);
    _syncByRevision.store(false);
  }

  TRI_ASSERT(info.isObject());

  bool extendedNames = vocbase.server()
                           .getFeature<DatabaseFeature>()
                           .extendedNamesForCollections();
  if (!CollectionNameValidator::isAllowedName(system(), extendedNames,
                                              name())) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_NAME);
  }

  if (_version < minimumVersion()) {
    // collection is too "old"
    std::string errorMsg(std::string("collection '") + name() +
                         "' has a too old version. Please start the server "
                         "with the --database.auto-upgrade option.");

    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FAILED, errorMsg);
  }

  if (auto res = updateSchema(info.get(StaticStrings::Schema)); res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  _internalValidatorTypes = Helper::getNumericValue<uint64_t>(
      info, StaticStrings::InternalValidatorTypes, 0);

  TRI_ASSERT(!guid().empty());

  // update server's tick value
  TRI_UpdateTickServer(id().id());

  // TODO: THIS NEEDS CLEANUP (Naming & Structural issue)
  initializeSmartAttributesBefore(info);

  _sharding = std::make_unique<ShardingInfo>(info, this);

  // TODO: THIS NEEDS CLEANUP (Naming & Structural issue)
  initializeSmartAttributesAfter(info);

  if (ServerState::instance()->isDBServer() ||
      !ServerState::instance()->isRunningInCluster()) {
    _followers = std::make_unique<FollowerInfo>(this);
  }

  TRI_ASSERT(_physical != nullptr);
  // This has to be called AFTER _physical and _logical are properly linked
  // together.

  prepareIndexes(info.get(StaticStrings::Indexes));
  decorateWithInternalValidators();

  // create key generator based on keyOptions from slice
  _keyGenerator = KeyGeneratorHelper::createKeyGenerator(
      *this, info.get(StaticStrings::KeyOptions));

  // computed values
  if (auto res = updateComputedValues(info.get(StaticStrings::ComputedValues));
      res.fail()) {
    LOG_TOPIC("4c73f", ERR, Logger::FIXME)
        << "collection '" << this->vocbase().name() << "/" << name() << ": "
        << res.errorMessage()
        << " - disabling computed values for this collection. original value: "
        << info.get(StaticStrings::ComputedValues).toJson();
    TRI_ASSERT(_computedValues == nullptr);
  }
}

LogicalCollection::~LogicalCollection() = default;

#ifndef USE_ENTERPRISE
void LogicalCollection::initializeSmartAttributesBefore(
    velocypack::Slice info) {
  // nothing to do in community edition
}
void LogicalCollection::initializeSmartAttributesAfter(velocypack::Slice info) {
  // nothing to do in community edition
}
#endif

Result LogicalCollection::updateSchema(VPackSlice schema) {
  if (!schema.isNone()) {
    if (schema.isNull()) {
      schema = VPackSlice::emptyObjectSlice();
    }
    if (!schema.isObject()) {
      return {TRI_ERROR_VALIDATION_BAD_PARAMETER,
              "Schema description is not an object."};
    }

    TRI_ASSERT(schema.isObject());

    std::shared_ptr<ValidatorBase> newSchema;

    // schema will be removed if empty object is given
    if (!schema.isEmptyObject()) {
      try {
        newSchema = std::make_shared<ValidatorJsonSchema>(schema);
      } catch (std::exception const& ex) {
        return {TRI_ERROR_VALIDATION_BAD_PARAMETER,
                absl::StrCat("Error when building schema: ", ex.what())};
      }
    }

    std::atomic_store_explicit(&_schema, newSchema, std::memory_order_release);
  }

  return {};
}

Result LogicalCollection::updateComputedValues(VPackSlice computedValues) {
  auto result =
      ComputedValues::buildInstance(vocbase(), shardKeys(), computedValues);

  if (result.fail()) {
    return result.result();
  }

  std::atomic_store_explicit(&_computedValues, result.get(),
                             std::memory_order_release);

  return {};
}

RevisionId LogicalCollection::newRevisionId() const {
  if (hasClusterWideUniqueRevs()) {
    auto& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
    return RevisionId::createClusterWideUnique(ci);
  }
  return RevisionId::create();
}

// SECTION: sharding
ShardingInfo* LogicalCollection::shardingInfo() const {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding.get();
}

size_t LogicalCollection::numberOfShards() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->numberOfShards();
}

size_t LogicalCollection::replicationFactor() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->replicationFactor();
}

size_t LogicalCollection::writeConcern() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->writeConcern();
}

replication::Version LogicalCollection::replicationVersion() const noexcept {
  return vocbase().replicationVersion();
}

std::string const& LogicalCollection::distributeShardsLike() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->distributeShardsLike();
}

void LogicalCollection::distributeShardsLike(std::string const& cid,
                                             ShardingInfo const* other) {
  TRI_ASSERT(_sharding != nullptr);
  _sharding->distributeShardsLike(cid, other);
}

std::vector<std::string> const& LogicalCollection::avoidServers()
    const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->avoidServers();
}

bool LogicalCollection::isSatellite() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->isSatellite();
}

bool LogicalCollection::usesDefaultShardKeys() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->usesDefaultShardKeys();
}

std::vector<std::string> const& LogicalCollection::shardKeys() const noexcept {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->shardKeys();
}

std::shared_ptr<ShardMap> LogicalCollection::shardIds() const {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->shardIds();
}

void LogicalCollection::setShardMap(std::shared_ptr<ShardMap> map) noexcept {
  TRI_ASSERT(_sharding != nullptr);
  _sharding->setShardMap(std::move(map));
}

ErrorCode LogicalCollection::getResponsibleShard(velocypack::Slice slice,
                                                 bool docComplete,
                                                 std::string& shardID) {
  bool usesDefaultShardKeys;
  return getResponsibleShard(slice, docComplete, shardID, usesDefaultShardKeys);
}

ErrorCode LogicalCollection::getResponsibleShard(std::string_view key,
                                                 std::string& shardID) {
  bool usesDefaultShardKeys;
  return getResponsibleShard(VPackSlice::emptyObjectSlice(), false, shardID,
                             usesDefaultShardKeys,
                             std::string_view(key.data(), key.size()));
}

ErrorCode LogicalCollection::getResponsibleShard(velocypack::Slice slice,
                                                 bool docComplete,
                                                 std::string& shardID,
                                                 bool& usesDefaultShardKeys,
                                                 std::string_view key) {
  TRI_ASSERT(_sharding != nullptr);
  return _sharding->getResponsibleShard(slice, docComplete, shardID,
                                        usesDefaultShardKeys, key);
}

void LogicalCollection::prepareIndexes(VPackSlice indexesSlice) {
  TRI_ASSERT(_physical != nullptr);

  if (!indexesSlice.isArray()) {
    // always point to an array
    indexesSlice = velocypack::Slice::emptyArraySlice();
  }

  _physical->prepareIndexes(indexesSlice);
}

std::unique_ptr<IndexIterator> LogicalCollection::getAllIterator(
    transaction::Methods* trx, ReadOwnWrites readOwnWrites) {
  return _physical->getAllIterator(trx, readOwnWrites);
}

std::unique_ptr<IndexIterator> LogicalCollection::getAnyIterator(
    transaction::Methods* trx) {
  return _physical->getAnyIterator(trx);
}
// @brief Return the number of documents in this collection
uint64_t LogicalCollection::numberDocuments(transaction::Methods* trx,
                                            transaction::CountType type) {
  // detailed results should have been handled in the levels above us
  TRI_ASSERT(type != transaction::CountType::Detailed);

  uint64_t documents = transaction::CountCache::NotPopulated;
  if (type == transaction::CountType::ForceCache) {
    // always return from the cache, regardless what's in it
    documents = _countCache.get();
  } else if (type == transaction::CountType::TryCache) {
    // get data from cache, but only if not expired
    documents = _countCache.getWithTtl();
  }
  if (documents == transaction::CountCache::NotPopulated) {
    // cache was not populated before or cache value has expired
    documents = getPhysical()->numberDocuments(trx);
    _countCache.store(documents);
  }
  TRI_ASSERT(documents != transaction::CountCache::NotPopulated);
  return documents;
}

bool LogicalCollection::hasClusterWideUniqueRevs() const noexcept {
  return usesRevisionsAsDocumentIds() && isSmartChild();
}

bool LogicalCollection::mustCreateKeyOnCoordinator() const noexcept {
  TRI_ASSERT(ServerState::instance()->isRunningInCluster());

#ifdef USE_ENTERPRISE
  if (_sharding->shardingStrategyName() ==
      ShardingStrategyEnterpriseHexSmartVertex::NAME) {
    TRI_ASSERT(isSmart());
    TRI_ASSERT(type() == TRI_COL_TYPE_DOCUMENT);
    TRI_ASSERT(!hasSmartGraphAttribute());
    // if we've a SmartVertex collection sitting inside an EnterpriseGraph
    // we need to have the key before we can call the "getResponsibleShards"
    // method because without it, we cannot calculate which shard will be the
    // responsible one.
    return true;
  }
#endif

  // when there is more than 1 shard, or if we do have a satellite
  // collection, we need to create the key on the coordinator.
  return numberOfShards() != 1;
}

uint32_t LogicalCollection::v8CacheVersion() const { return _v8CacheVersion; }

TRI_col_type_e LogicalCollection::type() const { return _type; }

TRI_vocbase_col_status_e LogicalCollection::status() const { return _status; }

TRI_vocbase_col_status_e LogicalCollection::getStatusLocked() {
  READ_LOCKER(readLocker, _statusLock);
  return _status;
}

void LogicalCollection::executeWhileStatusWriteLocked(
    std::function<void()> const& callback) {
  WRITE_LOCKER_EVENTUAL(locker, _statusLock);
  callback();
}

TRI_vocbase_col_status_e LogicalCollection::tryFetchStatus(bool& didFetch) {
  TRY_READ_LOCKER(locker, _statusLock);
  if (locker.isLocked()) {
    didFetch = true;
    return _status;
  }
  didFetch = false;
  return TRI_VOC_COL_STATUS_CORRUPTED;
}

// SECTION: Properties
RevisionId LogicalCollection::revision(transaction::Methods* trx) const {
  // TODO CoordinatorCase
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  return _physical->revision(trx);
}

#ifndef USE_ENTERPRISE
std::string const& LogicalCollection::smartJoinAttribute() const noexcept {
  return StaticStrings::Empty;
}
#endif

#ifndef USE_ENTERPRISE
std::string LogicalCollection::smartGraphAttribute() const {
  return StaticStrings::Empty;
}

void LogicalCollection::setSmartGraphAttribute(std::string const& /*value*/) {
  TRI_ASSERT(false);
}
#endif

bool LogicalCollection::usesRevisionsAsDocumentIds() const noexcept {
  return _usesRevisionsAsDocumentIds.load(std::memory_order_relaxed);
}

std::unique_ptr<FollowerInfo> const& LogicalCollection::followers() const {
  return _followers;
}

bool LogicalCollection::syncByRevision() const noexcept {
  return _syncByRevision.load();
}

bool LogicalCollection::useSyncByRevision() const noexcept {
  return !_isAStub && _syncByRevision.load();
}

bool LogicalCollection::determineSyncByRevision() const {
  if (version() >= LogicalCollection::Version::v37) {
    auto& server = vocbase().server();
    if (server.hasFeature<ReplicationFeature>()) {
      auto& replication = server.getFeature<ReplicationFeature>();
      return replication.syncByRevision() && usesRevisionsAsDocumentIds();
    }
  }
  return false;
}

std::vector<std::shared_ptr<Index>> LogicalCollection::getIndexes() const {
  return getPhysical()->getIndexes();
}

void LogicalCollection::getIndexesVPack(
    VPackBuilder& result,
    std::function<bool(Index const*,
                       std::underlying_type<Index::Serialize>::type&)> const&
        filter) const {
  getPhysical()->getIndexesVPack(result, filter);
}

bool LogicalCollection::allowUserKeys() const noexcept {
  return _allowUserKeys;
}

// SECTION: Modification Functions

// asks the storage engine to rename the collection to the given name
// and persist the renaming info. It is guaranteed by the server
// that no other active collection with the same name and id exists in the same
// database when this function is called. If this operation fails somewhere in
// the middle, the storage engine is required to fully revert the rename
// operation
// and throw only then, so that subsequent collection creation/rename requests
// will
// not fail. the WAL entry for the rename will be written *after* the call
// to "renameCollection" returns

Result LogicalCollection::rename(std::string&& newName) {
  // Should only be called from inside vocbase.
  // Otherwise caching is destroyed.
  TRI_ASSERT(!ServerState::instance()->isCoordinator());  // NOT YET IMPLEMENTED

  if (!vocbase().server().hasFeature<DatabaseFeature>()) {
    return Result(
        TRI_ERROR_INTERNAL,
        "failed to find feature 'Database' while renaming collection");
  }

  // Check for illegal states.
  if (_status != TRI_VOC_COL_STATUS_LOADED) {
    if (_status == TRI_VOC_COL_STATUS_CORRUPTED) {
      return TRI_ERROR_ARANGO_CORRUPTED_COLLECTION;
    } else if (_status == TRI_VOC_COL_STATUS_DELETED) {
      return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
    }
    // Unknown status
    return TRI_ERROR_INTERNAL;
  }

  std::string oldName = name();

  // Okay we can finally rename safely
  try {
    StorageEngine& engine =
        vocbase().server().getFeature<EngineSelectorFeature>().engine();
    name(std::move(newName));
    engine.changeCollection(vocbase(), *this);
  } catch (basics::Exception const& ex) {
    // Engine Rename somehow failed. Reset to old name
    name(std::move(oldName));

    return ex.code();
  } catch (...) {
    // Engine Rename somehow failed. Reset to old name
    name(std::move(oldName));

    return {TRI_ERROR_INTERNAL};
  }

  // CHECK if this ordering is okay. Before change the version was increased
  // after swapping in vocbase mapping.
  increaseV8Version();
  return {};
}

ErrorCode LogicalCollection::close() { return getPhysical()->close(); }

Result LogicalCollection::drop() {
  // make sure collection has been closed
  this->close();

  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  deleted(true);
  _physical->drop();

  return {};
}

void LogicalCollection::setStatus(TRI_vocbase_col_status_e status) {
  _status = status;

  if (status == TRI_VOC_COL_STATUS_LOADED) {
    increaseV8Version();
  }
}

void LogicalCollection::toVelocyPackForInventory(VPackBuilder& result) const {
  result.openObject();
  result.add(VPackValue(StaticStrings::Indexes));
  getIndexesVPack(result, [](arangodb::Index const* idx,
                             decltype(Index::makeFlags())& flags) {
    // we have to exclude the primary and edge index for dump / restore
    switch (idx->type()) {
      case Index::TRI_IDX_TYPE_PRIMARY_INDEX:
      case Index::TRI_IDX_TYPE_EDGE_INDEX:
        return false;
      default:
        flags = Index::makeFlags(Index::Serialize::Inventory);
        return !idx->isHidden();
    }
  });
  result.add("parameters", VPackValue(VPackValueType::Object));
  toVelocyPackIgnore(
      result,
      {StaticStrings::ObjectId, "path", "statusString", StaticStrings::Indexes},
      LogicalDataSource::Serialization::Inventory);
  result.close();  // parameters
  result.close();  // collection
}

void LogicalCollection::toVelocyPackForClusterInventory(VPackBuilder& result,
                                                        bool useSystem,
                                                        bool isReady,
                                                        bool allInSync) const {
  TRI_ASSERT(_sharding != nullptr);
  if (system() && !useSystem) {
    return;
  }

  result.openObject();
  result.add(VPackValue("parameters"));

  std::unordered_set<std::string> ignoreKeys{
      StaticStrings::AllowUserKeys,
      StaticStrings::DataSourceCid,
      "count",
      "statusString",
      StaticStrings::Version,
      StaticStrings::DistributeShardsLike,
      StaticStrings::ObjectId,
      StaticStrings::Indexes};
  VPackBuilder params = toVelocyPackIgnore(ignoreKeys, Serialization::List);
  {
    VPackObjectBuilder guard(&result);

    for (auto const& p : VPackObjectIterator(params.slice())) {
      result.add(p.key);
      result.add(p.value);
    }

    if (!_sharding->distributeShardsLike().empty()) {
      CollectionNameResolver resolver(vocbase());

      result.add(StaticStrings::DistributeShardsLike,
                 VPackValue(resolver.getCollectionNameCluster(DataSourceId{
                     basics::StringUtils::uint64(distributeShardsLike())})));
    }
  }

  result.add(VPackValue(StaticStrings::Indexes));
  getIndexesVPack(result, [](Index const* idx, uint8_t& flags) {
    // we have to exclude the primary and the edge index here, because otherwise
    // at least the MMFiles engine will try to create it
    // AND exclude hidden indexes
    switch (idx->type()) {
      case Index::TRI_IDX_TYPE_PRIMARY_INDEX:
      case Index::TRI_IDX_TYPE_EDGE_INDEX:
        return false;
      default:
        flags = Index::makeFlags(Index::Serialize::Inventory);
        return !idx->isHidden() && !idx->inProgress();
    }
  });
  // planVersion is hard-coded to 1 since 3.8
  result.add("planVersion", VPackValue(1));
  result.add("isReady", VPackValue(isReady));
  result.add("allInSync", VPackValue(allInSync));
  result.close();  // CollectionInfo
}

Result LogicalCollection::appendVPack(velocypack::Builder& build,
                                      Serialization ctx, bool) const {
  TRI_ASSERT(_sharding != nullptr);
  bool const forPersistence = (ctx == Serialization::Persistence ||
                               ctx == Serialization::PersistenceWithInProgress);
  bool const showInProgress = (ctx == Serialization::PersistenceWithInProgress);
  // We write into an open object
  TRI_ASSERT(build.isOpenObject());
  // Collection Meta Information
  build.add(StaticStrings::DataSourceCid,
            VPackValue(std::to_string(id().id())));
  build.add(StaticStrings::DataSourceType, VPackValue(static_cast<int>(_type)));
  build.add("status", VPackValue(_status));
  build.add("statusString", VPackValue(::translateStatus(_status)));
  build.add(StaticStrings::Version,
            VPackValue(static_cast<uint32_t>(_version)));
  // Collection Flags
  build.add(StaticStrings::WaitForSyncString, VPackValue(_waitForSync));
  if (!forPersistence) {
    // with 'forPersistence' added by LogicalDataSource::toVelocyPack
    // FIXME TODO is this needed in !forPersistence???
    build.add(StaticStrings::DataSourceDeleted, VPackValue(deleted()));
    build.add(StaticStrings::DataSourceSystem, VPackValue(system()));
  }
  // TODO is this still releveant or redundant in keyGenerator?
  build.add(StaticStrings::AllowUserKeys, VPackValue(_allowUserKeys));

  // keyoptions
  build.add(StaticStrings::KeyOptions, VPackValue(VPackValueType::Object));
  keyGenerator().toVelocyPack(build);
  build.close();

  // Physical Information
  getPhysical()->getPropertiesVPack(build);
  // Indexes
  build.add(VPackValue(StaticStrings::Indexes));
  auto indexFlags = Index::makeFlags();
  // hide hidden indexes. In effect hides unfinished indexes,
  // and iResearch links (only on a single-server and coordinator)
  if (forPersistence) {
    indexFlags = Index::makeFlags(Index::Serialize::Internals);
  }
  auto filter = [indexFlags, forPersistence, showInProgress](
                    Index const* idx, decltype(Index::makeFlags())& flags) {
    if ((forPersistence || !idx->isHidden()) &&
        (showInProgress || !idx->inProgress())) {
      flags = indexFlags;
      return true;
    }
    return false;
  };
  getIndexesVPack(build, filter);

  // Schema
  build.add(VPackValue(StaticStrings::Schema));
  schemaToVelocyPack(build);

  // Computed Values
  build.add(VPackValue(StaticStrings::ComputedValues));
  computedValuesToVelocyPack(build);

  // Internal CollectionType
  build.add(StaticStrings::InternalValidatorTypes,
            VPackValue(_internalValidatorTypes));
  // Cluster Specific
  build.add(StaticStrings::IsDisjoint, VPackValue(isDisjoint()));
  build.add(StaticStrings::IsSmart, VPackValue(isSmart()));
  build.add(StaticStrings::IsSmartChild, VPackValue(isSmartChild()));
  build.add(StaticStrings::UsesRevisionsAsDocumentIds,
            VPackValue(usesRevisionsAsDocumentIds()));
  build.add(StaticStrings::SyncByRevision, VPackValue(syncByRevision()));
  if (!forPersistence) {
    // with 'forPersistence' added by LogicalDataSource::toVelocyPack
    // TODO is this needed in !forPersistence???
    build.add(StaticStrings::DataSourcePlanId,
              VPackValue(std::to_string(planId().id())));
  }
  _sharding->toVelocyPack(build, ctx != Serialization::List);
  includeVelocyPackEnterprise(build);
  TRI_ASSERT(build.isOpenObject());
  // We leave the object open
  return {};
}

void LogicalCollection::toVelocyPackIgnore(
    VPackBuilder& result, std::unordered_set<std::string> const& ignoreKeys,
    Serialization context) const {
  TRI_ASSERT(result.isOpenObject());
  VPackBuilder b = toVelocyPackIgnore(ignoreKeys, context);
  result.add(VPackObjectIterator(b.slice()));
}

VPackBuilder LogicalCollection::toVelocyPackIgnore(
    std::unordered_set<std::string> const& ignoreKeys,
    Serialization context) const {
  VPackBuilder full;
  full.openObject();
  properties(full, context);
  full.close();
  if (ignoreKeys.empty()) {
    return full;
  }
  return VPackCollection::remove(full.slice(), ignoreKeys);
}

#ifndef USE_ENTERPRISE
void LogicalCollection::includeVelocyPackEnterprise(
    VPackBuilder& result) const {
  // We ain't no Enterprise Edition
}
#endif

void LogicalCollection::increaseV8Version() { ++_v8CacheVersion; }

Result LogicalCollection::properties(velocypack::Slice slice) {
  TRI_ASSERT(_sharding != nullptr);
  // the following collection properties are intentionally not updated,
  // as updating them would be very complicated:
  // - _cid
  // - _name
  // - _type
  // - _isSystem
  // ... probably a few others missing here ...

  if (!vocbase().server().hasFeature<DatabaseFeature>()) {
    return Result(
        TRI_ERROR_INTERNAL,
        "failed to find feature 'Database' while updating collection");
  }

  if (!vocbase().server().hasFeature<EngineSelectorFeature>() ||
      !vocbase().server().getFeature<EngineSelectorFeature>().selected()) {
    return Result(TRI_ERROR_INTERNAL,
                  "failed to find a storage engine while updating collection");
  }
  auto& engine =
      vocbase().server().getFeature<EngineSelectorFeature>().engine();

  MUTEX_LOCKER(guard, _infoLock);  // prevent simultaneous updates

  auto res = updateSchema(slice.get(StaticStrings::Schema));
  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  res = updateComputedValues(slice.get(StaticStrings::ComputedValues));
  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  size_t replicationFactor = _sharding->replicationFactor();
  size_t writeConcern = _sharding->writeConcern();
  VPackSlice replicationFactorSlice =
      slice.get(StaticStrings::ReplicationFactor);

  VPackSlice writeConcernSlice = slice.get(StaticStrings::WriteConcern);
  if (writeConcernSlice.isNone()) {  // deprecated in 3.6
    writeConcernSlice = slice.get(StaticStrings::MinReplicationFactor);
  }

  if (!replicationFactorSlice.isNone()) {
    if (replicationFactorSlice.isInteger()) {
      int64_t replicationFactorTest =
          replicationFactorSlice.getNumber<int64_t>();
      if (replicationFactorTest < 0) {
        // negative value for replication factor... not good
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "bad value for replicationFactor");
      }

      auto& cf = vocbase().server().getFeature<ClusterFeature>();
      replicationFactor = replicationFactorSlice.getNumber<size_t>();
      if ((!isSatellite() && replicationFactor == 0) ||
          (ServerState::instance()->isCoordinator() &&
           (replicationFactor < cf.minReplicationFactor() ||
            replicationFactor > cf.maxReplicationFactor()))) {
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "bad value for replicationFactor");
      }

      if (ServerState::instance()->isCoordinator() &&
          replicationFactor !=
              _sharding->replicationFactor()) {  // check if changed
        if (!_sharding->distributeShardsLike().empty()) {
          return Result(TRI_ERROR_FORBIDDEN,
                        "cannot change replicationFactor for a collection "
                        "using 'distributeShardsLike'");
        } else if (_type == TRI_COL_TYPE_EDGE && isSmart()) {
          return Result(TRI_ERROR_NOT_IMPLEMENTED,
                        "changing replicationFactor is "
                        "not supported for smart edge collections");
        } else if (isSatellite()) {
          return Result(
              TRI_ERROR_FORBIDDEN,
              "cannot change replicationFactor of a SatelliteCollection");
        }
      }
    } else if (replicationFactorSlice.isString()) {
      if (replicationFactorSlice.stringView() != StaticStrings::Satellite) {
        // only the string "satellite" is allowed here
        return Result(TRI_ERROR_BAD_PARAMETER,
                      "bad value for replicationFactor. expecting 'satellite'");
      }
      // we got the string "satellite"...
#ifdef USE_ENTERPRISE
      if (!isSatellite()) {
        // but the collection is not a SatelliteCollection!
        return Result(TRI_ERROR_FORBIDDEN,
                      "cannot change SatelliteCollection status");
      }
#else
      return Result(TRI_ERROR_FORBIDDEN,
                    "cannot use SatelliteCollection status");
#endif
      // fallthrough here if we set the string "satellite" for a satellite
      // collection
      TRI_ASSERT(isSatellite() && _sharding->replicationFactor() == 0 &&
                 replicationFactor == 0);
    } else {
      return Result(TRI_ERROR_BAD_PARAMETER, "bad value for replicationFactor");
    }
  }

  if (!writeConcernSlice.isNone()) {
    if (writeConcernSlice.isInteger()) {
      int64_t writeConcernTest = writeConcernSlice.getNumber<int64_t>();
      if (writeConcernTest < 0) {
        // negative value for writeConcern... not good
        return Result(TRI_ERROR_BAD_PARAMETER, "bad value for writeConcern");
      }

      writeConcern = writeConcernSlice.getNumber<size_t>();
      if (writeConcern > replicationFactor) {
        return Result(TRI_ERROR_BAD_PARAMETER, "bad value for writeConcern");
      }

      if ((ServerState::instance()->isCoordinator() ||
           (ServerState::instance()->isSingleServer() &&
            (isSatellite() || isSmart()))) &&
          writeConcern != _sharding->writeConcern()) {  // check if changed
        if (!_sharding->distributeShardsLike().empty()) {
          return Result(TRI_ERROR_FORBIDDEN,
                        "Cannot change writeConcern, please change " +
                            _sharding->distributeShardsLike());
        } else if (_type == TRI_COL_TYPE_EDGE && isSmart()) {
          return Result(TRI_ERROR_NOT_IMPLEMENTED,
                        "Changing writeConcern "
                        "not supported for smart edge collections");
        } else if (isSatellite()) {
          return Result(TRI_ERROR_FORBIDDEN,
                        "SatelliteCollection, "
                        "cannot change writeConcern");
        }
      }
    } else {
      return Result(TRI_ERROR_BAD_PARAMETER, "bad value for writeConcern");
    }
    TRI_ASSERT((writeConcern <= replicationFactor && !isSatellite()) ||
               (writeConcern == 0 && isSatellite()));
  }

  // The physical may first reject illegal properties.
  // After this call it either has thrown or the properties are stored
  res = getPhysical()->updateProperties(slice);
  if (!res.ok()) {
    return res;
  }

  TRI_ASSERT(!isSatellite() || replicationFactor == 0);
  _waitForSync = Helper::getBooleanValue(
      slice, StaticStrings::WaitForSyncString, _waitForSync);
  _sharding->setWriteConcernAndReplicationFactor(writeConcern,
                                                 replicationFactor);

  if (ServerState::instance()->isDBServer()) {
    // This code is only allowed to be executed by the maintenance
    // and should only be triggered during cluster upgrades.
    // The user is NOT allowed to modify this value in any way.
    auto nextType = Helper::getNumericValue<uint64_t>(
        slice, StaticStrings::InternalValidatorTypes, _internalValidatorTypes);
    if (nextType != _internalValidatorTypes) {
      // This is a bit dangerous operation, if the internalValidators are NOT
      // empty a concurrent writer could have one in its hand while this thread
      // deletes it. For now the situation cannot happen, but may happen in the
      // future. As soon as it happens we need to make sure that we hold a write
      // lock on this collection while we swap the validators. (Or apply some
      // local locking for validators only, that would be fine too)

      TRI_ASSERT(_internalValidators.empty());
      _internalValidatorTypes = nextType;
      _internalValidators.clear();
      decorateWithInternalValidators();
    }

    // Inject SmartGraphAttribute into Shards
    if (slice.hasKey(StaticStrings::GraphSmartGraphAttribute) &&
        smartGraphAttribute().empty()) {
      // This is a bit dangerous operation and should only be run in MAINTENANCE
      // task we upgrade the SmartGraphAttribute within the Shard, this is
      // required to allow the DBServer to generate new ID values
      auto sga = slice.get(StaticStrings::GraphSmartGraphAttribute);
      if (sga.isString()) {
        setSmartGraphAttribute(sga.copyString());
      }
    }
  }

  if (ServerState::instance()->isCoordinator()) {
    // We need to inform the cluster as well
    auto& ci = vocbase().server().getFeature<ClusterFeature>().clusterInfo();
    return ci.setCollectionPropertiesCoordinator(
        vocbase().name(), std::to_string(id().id()), this);
  }

  engine.changeCollection(vocbase(), *this);

  auto& df = vocbase().server().getFeature<DatabaseFeature>();
  if (df.versionTracker() != nullptr) {
    df.versionTracker()->track("change collection");
  }

  return {};
}

/// @brief return the figures for a collection
futures::Future<OperationResult> LogicalCollection::figures(
    bool details, OperationOptions const& options) const {
  return getPhysical()->figures(details, options);
}

/// SECTION Indexes

std::shared_ptr<Index> LogicalCollection::lookupIndex(IndexId idxId) const {
  return getPhysical()->lookupIndex(idxId);
}

std::shared_ptr<Index> LogicalCollection::lookupIndex(
    std::string_view idxName) const {
  return getPhysical()->lookupIndex(idxName);
}

std::shared_ptr<Index> LogicalCollection::lookupIndex(VPackSlice info) const {
  if (!info.isObject()) {
    // Compatibility with old v8-vocindex.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }
  return getPhysical()->lookupIndex(info);
}

std::shared_ptr<Index> LogicalCollection::createIndex(VPackSlice info,
                                                      bool& created) {
  auto idx = _physical->createIndex(info, /*restore*/ false, created);
  if (idx) {
    auto& df = vocbase().server().getFeature<DatabaseFeature>();
    if (df.versionTracker() != nullptr) {
      df.versionTracker()->track("create index");
    }
  }
  return idx;
}

Result LogicalCollection::updateIndex(IndexId iid, velocypack::Slice body,
                                      VPackBuilder& updated) {
  auto res = _physical->updateIndex(iid, body, updated);
  if (res.ok()) {
    auto& df = vocbase().server().getFeature<DatabaseFeature>();
    if (df.versionTracker() != nullptr) {
      df.versionTracker()->track("update index");
    }
  }
  return res;
}

/// @brief drops an index, including index file removal and replication
bool LogicalCollection::dropIndex(IndexId iid) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  aql::QueryCache::instance()->invalidate(&vocbase(), guid());

  bool result = _physical->dropIndex(iid);

  if (result) {
    auto& df = vocbase().server().getFeature<DatabaseFeature>();
    if (df.versionTracker() != nullptr) {
      df.versionTracker()->track("drop index");
    }
  }

  return result;
}

/// @brief Persist the connected physical collection.
///        This should be called AFTER the collection is successfully
///        created and only on Single/DBServer
void LogicalCollection::persistPhysicalCollection() {
  // Coordinators are not allowed to have local collections!
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  StorageEngine& engine =
      vocbase().server().getFeature<EngineSelectorFeature>().engine();
  engine.createCollection(vocbase(), *this);
}

basics::ReadWriteLock& LogicalCollection::statusLock() noexcept {
  return _statusLock;
}

/// @brief Defer a callback to be executed when the collection
///        can be dropped. The callback is supposed to drop
///        the collection and it is guaranteed that no one is using
///        it at that moment.
void LogicalCollection::deferDropCollection(
    std::function<bool(LogicalCollection&)> const& callback) {
  _syncByRevision = false;  // safety to make sure we can do physical cleanup
  _physical->deferDropCollection(callback);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief processes a truncate operation
////////////////////////////////////////////////////////////////////////////////

Result LogicalCollection::truncate(transaction::Methods& trx,
                                   OperationOptions& options,
                                   bool& usedRangeDelete) {
  TRI_IF_FAILURE("LogicalCollection::truncate") { return {TRI_ERROR_DEBUG}; }
  return getPhysical()->truncate(trx, options, usedRangeDelete);
}

/// @brief compact-data operation
void LogicalCollection::compact() { getPhysical()->compact(); }

/// @brief a method to skip certain documents in AQL write operations,
/// this is only used in the Enterprise Edition for SmartGraphs
#ifndef USE_ENTERPRISE
bool LogicalCollection::skipForAqlWrite(velocypack::Slice document,
                                        std::string const& key) const {
  return false;
}
#endif

void LogicalCollection::computedValuesToVelocyPack(VPackBuilder& b) const {
  auto cv = computedValues();
  if (cv != nullptr) {
    cv->toVelocyPack(b);
  } else {
    b.add(VPackSlice::nullSlice());
  }
}

std::shared_ptr<ComputedValues> LogicalCollection::computedValues() const {
  return std::atomic_load_explicit(&_computedValues, std::memory_order_acquire);
}

void LogicalCollection::schemaToVelocyPack(VPackBuilder& b) const {
  auto s = schema();
  if (s != nullptr) {
    s->toVelocyPack(b);
  } else {
    b.add(VPackSlice::nullSlice());
  }
}

std::shared_ptr<ValidatorBase> LogicalCollection::schema() const {
  return std::atomic_load_explicit(&_schema, std::memory_order_acquire);
}

Result LogicalCollection::validate(std::shared_ptr<ValidatorBase> const& schema,
                                   VPackSlice s,
                                   VPackOptions const* options) const {
  if (schema != nullptr) {
    auto res = schema->validate(s, VPackSlice::noneSlice(), true, options);
    if (res.fail()) {
      return res;
    }
  }
  for (auto const& validator : _internalValidators) {
    auto res = validator->validate(s, VPackSlice::noneSlice(), true, options);
    if (!res.ok()) {
      return res;
    }
  }
  return {};
}

Result LogicalCollection::validate(std::shared_ptr<ValidatorBase> const& schema,
                                   VPackSlice modifiedDoc, VPackSlice oldDoc,
                                   VPackOptions const* options) const {
  if (schema != nullptr) {
    auto res = schema->validate(modifiedDoc, oldDoc, false, options);
    if (res.fail()) {
      return res;
    }
  }
  for (auto const& validator : _internalValidators) {
    auto res = validator->validate(modifiedDoc, oldDoc, false, options);
    if (res.fail()) {
      return res;
    }
  }
  return {};
}

void LogicalCollection::setInternalValidatorTypes(uint64_t type) {
  _internalValidatorTypes = type;
}

uint64_t LogicalCollection::getInternalValidatorTypes() const noexcept {
  return _internalValidatorTypes;
}

void LogicalCollection::addInternalValidator(
    std::unique_ptr<ValidatorBase> validator) {
  // For the time beeing we only allow ONE internal validator.
  // This however is a non-necessary restriction and can be leveraged at any
  // time. The code is prepared to handle any number of validators, and this
  // assert is only to make sure we do not create one twice.
  TRI_ASSERT(_internalValidators.empty());
  TRI_ASSERT(validator);
  _internalValidators.emplace_back(std::move(validator));
}

void LogicalCollection::decorateWithInternalValidators() {
  // Community validators go in here.
  decorateWithInternalEEValidators();
}

replication2::LogId LogicalCollection::shardIdToStateId(
    std::string_view shardId) {
  auto logId = tryShardIdToStateId(shardId);
  ADB_PROD_ASSERT(logId.has_value())
      << " converting " << shardId << " to LogId failed";
  return logId.value();
}

std::optional<replication2::LogId> LogicalCollection::tryShardIdToStateId(
    std::string_view shardId) {
  if (shardId.empty()) {
    return {};
  }
  auto stateId = shardId.substr(1, shardId.size() - 1);
  return replication2::LogId::fromString(stateId);
}

auto LogicalCollection::getDocumentState()
    -> std::shared_ptr<replication2::replicated_state::ReplicatedState<
        replication2::replicated_state::document::DocumentState>> {
  using namespace replication2::replicated_state;
  auto stateMachine =
      std::dynamic_pointer_cast<ReplicatedState<document::DocumentState>>(
          vocbase().getReplicatedStateById(shardIdToStateId(name())));
  ADB_PROD_ASSERT(stateMachine != nullptr)
      << "Missing document state in shard " << name();
  return stateMachine;
}

auto LogicalCollection::getDocumentStateLeader() -> std::shared_ptr<
    replication2::replicated_state::document::DocumentLeaderState> {
  auto stateMachine = getDocumentState();

  static constexpr auto throwUnavailable = []<typename... Args>(
      basics::SourceLocation location, fmt::format_string<Args...> formatString,
      Args && ... args) {
    throw basics::Exception(
        TRI_ERROR_REPLICATION_REPLICATED_STATE_NOT_AVAILABLE,
        fmt::vformat(formatString, fmt::make_format_args(args...)), location);
  };

  auto const status = stateMachine->getStatus();
  if (status == std::nullopt) {
    throwUnavailable(ADB_HERE,
                     "Replicated state {} is not available, accessed "
                     "from {}/{}. No status available.",
                     shardIdToStateId(name()), vocbase().name(), name());
  }

  auto const* const leaderStatus = status->asLeaderStatus();
  if (leaderStatus == nullptr) {
    throwUnavailable(ADB_HERE,
                     "Replicated state {} is not available as leader, accessed "
                     "from {}/{}. Status is {}.",
                     shardIdToStateId(name()), vocbase().name(), name(),
                     fmt::streamed(*status));
  }

  auto leader = stateMachine->getLeader();
  if (leader == nullptr) {
    throwUnavailable(ADB_HERE,
                     "Replicated state {} is not available as leader, accessed "
                     "from {}/{}. Status is {}.",
                     shardIdToStateId(name()), vocbase().name(), name(),
                     to_string(leaderStatus->managerState.state));
  }

  return leader;
}

auto LogicalCollection::getDocumentStateFollower() -> std::shared_ptr<
    replication2::replicated_state::document::DocumentFollowerState> {
  auto stateMachine = getDocumentState();
  auto follower = stateMachine->getFollower();
  // TODO improve error handling
  ADB_PROD_ASSERT(follower != nullptr)
      << "Cannot get DocumentFollowerState in shard " << name();
  return follower;
}

#ifndef USE_ENTERPRISE
void LogicalCollection::decorateWithInternalEEValidators() {
  // Only available in Enterprise Mode
}
#endif
