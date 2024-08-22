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
/// @author Michael Hackstein
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Basics/ReadWriteLock.h"
#include "Containers/FlatHashMap.h"
#include "Cluster/Utils/ShardID.h"
#include "Futures/Future.h"
#include "Indexes/IndexIterator.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/StateMachines/Document/CreateIndexReplicationCallback.h"
#include "Transaction/CountCache.h"
#include "Utils/OperationResult.h"
#include "VocBase/Identifiers/IndexId.h"
#include "VocBase/Identifiers/RevisionId.h"
#include "VocBase/LogicalDataSource.h"
#include "VocBase/Validators.h"
#include "VocBase/voc-types.h"

#include <mutex>

namespace arangodb {

namespace velocypack {
class Builder;
class Slice;
}  // namespace velocypack
typedef std::string ServerID;  // ID of a server
using ShardMap = containers::FlatHashMap<ShardID, std::vector<ServerID>>;

struct UserInputCollectionProperties;
class ComputedValues;
class FollowerInfo;
class Index;
class IndexIterator;
class KeyGenerator;
class LocalDocumentId;
struct OperationOptions;
class PhysicalCollection;
class Result;
class ShardingInfo;

namespace transaction {
class Methods;
}

namespace replication {
enum class Version;
}
namespace replication2 {
namespace replicated_state {
template<typename S>
struct ReplicatedState;
namespace document {
struct DocumentState;
struct DocumentLeaderState;
struct DocumentFollowerState;
}  // namespace document
}  // namespace replicated_state
namespace agency {
struct CollectionGroupId;
}
}  // namespace replication2

/// please note that coordinator-based logical collections are frequently
/// created and discarded, so ctor & dtor need to be as efficient as possible.
/// additionally, do not put any volatile state into this object in the
/// coordinator, as the ClusterInfo may create many different temporary physical
/// LogicalCollection objects (one after the other) even for the same "logical"
/// LogicalCollection. this which will also discard the collection's volatile
/// state each time! all state of a LogicalCollection in the coordinator case
/// needs to be derived from the JSON info in the agency's plan entry for the
/// collection...

class LogicalCollection : public LogicalDataSource {
  friend struct ::TRI_vocbase_t;

 public:
  LogicalCollection() = delete;
  LogicalCollection(TRI_vocbase_t& vocbase, velocypack::Slice info,
                    bool isAStub);
  LogicalCollection(LogicalCollection const&) = delete;
  LogicalCollection& operator=(LogicalCollection const&) = delete;
  ~LogicalCollection() override;

  enum class Version { v30 = 5, v31 = 6, v33 = 7, v34 = 8, v37 = 9 };

  constexpr static Category category() noexcept {
    return Category::kCollection;
  }

  /*
   * @brief Available types of internal validators. These validators
   * are managed by the database, and bound to features of collections.
   * They cannot be modified by the user, and should therefore not be exposed.
   * Mostly used for Enterprise/Smart collection types which have
   * some specialities over community collections.
   * This enum is used to generate bitmap entries. So whenever you
   * add a new value make sure it is the next free 2^n value.
   * For Backwards Compatibility a value can never be reused.
   */
  enum InternalValidatorType : std::uint64_t {
    None = 0,
    LogicalSmartEdge = 1,
    LocalSmartEdge = 2,
    RemoteSmartEdge = 4,
    SmartToSatEdge = 8,
    SatToSmartEdge = 16,
  };

  /// @brief hard-coded minimum version number for collections
  static constexpr Version minimumVersion() { return Version::v30; }
  /// @brief current version for collections
  static constexpr Version currentVersion() { return Version::v37; }

  static replication2::LogId shardIdToStateId(ShardID const& shardId);
  static std::optional<replication2::LogId> tryShardIdToStateId(
      ShardID const& shardId);

  // SECTION: Meta Information
  Version version() const noexcept { return _version; }

  void setVersion(Version version) noexcept { _version = version; }

  uint32_t v8CacheVersion() const noexcept { return _v8CacheVersion; }

  TRI_col_type_e type() const noexcept { return _type; }

  // For normal collections the realNames is just a vector of length 1
  // with its name. For smart edge collections (Enterprise Edition only)
  // this is different.
  virtual std::vector<std::string> realNames() const {
    return std::vector<std::string>{name()};
  }
  // Same here, this is for reading in AQL:
  virtual std::vector<std::string> realNamesForRead() const {
    return std::vector<std::string>{name()};
  }

  RevisionId newRevisionId() const;

  void executeWhileStatusWriteLocked(std::function<void()> const& callback);

  // SECTION: Properties
  RevisionId revision(transaction::Methods*) const;
  bool waitForSync() const noexcept;
  bool cacheEnabled() const noexcept;
#ifdef USE_ENTERPRISE
  bool isDisjoint() const noexcept { return _isDisjoint; }
  bool isSmart() const noexcept { return _isSmart; }
  bool isSmartChild() const noexcept { return _isSmartChild; }
  bool hasSmartJoinAttribute() const noexcept {
    return !_smartJoinAttribute.empty();
  }
  bool hasSmartGraphAttribute() const noexcept {
    return !_smartGraphAttribute.empty();
  }

  bool isLocalSmartEdgeCollection() const noexcept;
  bool isRemoteSmartEdgeCollection() const noexcept;
  bool isSmartEdgeCollection() const noexcept;
  bool isSatToSmartEdgeCollection() const noexcept;
  bool isSmartToSatEdgeCollection() const noexcept;
  bool isSmartVertexCollection() const noexcept;

#else
  bool isDisjoint() const noexcept { return false; }
  bool isSmart() const noexcept { return false; }
  bool isSmartChild() const noexcept { return false; }
  bool hasSmartJoinAttribute() const noexcept { return false; }
  bool hasSmartGraphAttribute() const noexcept { return false; }

  bool isLocalSmartEdgeCollection() const noexcept { return false; }
  bool isRemoteSmartEdgeCollection() const noexcept { return false; }
  bool isSmartEdgeCollection() const noexcept { return false; }
  bool isSatToSmartEdgeCollection() const noexcept { return false; }
  bool isSmartToSatEdgeCollection() const noexcept { return false; }
#endif

  /// @brief return the name of the SmartJoin attribute (empty string
  /// if no SmartJoin attribute is present)
  std::string const& smartJoinAttribute() const noexcept;

  std::string smartGraphAttribute() const;
  void setSmartGraphAttribute(std::string const& value);

  bool usesRevisionsAsDocumentIds() const noexcept;

  /// @brief is this a cluster-wide Plan (ClusterInfo) collection
  bool isAStub() const noexcept { return _isAStub; }

  bool hasClusterWideUniqueRevs() const noexcept;

  [[nodiscard]] bool mustCreateKeyOnCoordinator() const noexcept;

  // SECTION: sharding
  ShardingInfo* shardingInfo() const;

  UserInputCollectionProperties getCollectionProperties() const noexcept;

  // proxy methods that will use the sharding info in the background
  size_t numberOfShards() const noexcept;
  size_t replicationFactor() const noexcept;
  size_t writeConcern() const noexcept;
  replication::Version replicationVersion() const noexcept;
  std::string distributeShardsLike() const noexcept;
  bool isSatellite() const noexcept;
  bool usesDefaultShardKeys() const noexcept;
  std::vector<std::string> const& shardKeys() const noexcept;

  virtual std::shared_ptr<ShardMap> shardIds() const;

  // @brief will write the full ShardMap into the builder, containing
  // all ShardIDs including their server distribution (ServerIDs) as an Object.
  // Example:
  //  {
  //    ...
  //    "s123456": ["PRMR-a41b97a0-e4d3-482b-925a-ff8efc9e0198", ...]
  //    ...
  //  }
  void shardMapToVelocyPack(arangodb::velocypack::Builder& result) const;

  // @brief will iterate over the whole ShardMap and will write only the
  // ShardIDs into the builder as an Array. The ShardIDs are sorted and returned
  // alphabetically.
  // Example:
  //  [
  //    ...
  //    "s123456",
  //    ...
  //  ]
  void shardIDsToVelocyPack(arangodb::velocypack::Builder& result) const;

  // mutation options for sharding
  void setShardMap(std::shared_ptr<ShardMap> map) noexcept;

  // query shard for a given document
  ResultT<ShardID> getResponsibleShard(velocypack::Slice slice,
                                       bool docComplete);
  ResultT<ShardID> getResponsibleShard(std::string_view key);

  ResultT<ShardID> getResponsibleShard(
      velocypack::Slice slice, bool docComplete, bool& usesDefaultShardKeys,
      std::string_view key = std::string_view());

  /**
   * Test if this Logical collection is the leading shard.
   * Will only return true on DBServers and Shards, and only
   * if they are leading. Independent of the Replication version
   */
  auto isLeadingShard() const -> bool;

  auto getDocumentState() const
      -> std::shared_ptr<replication2::replicated_state::ReplicatedState<
          replication2::replicated_state::document::DocumentState>>;
  auto getDocumentStateLeader() -> std::shared_ptr<
      replication2::replicated_state::document::DocumentLeaderState>;
  auto getDocumentStateFollower() -> std::shared_ptr<
      replication2::replicated_state::document::DocumentFollowerState>;

  PhysicalCollection* getPhysical() const { return _physical.get(); }

  std::unique_ptr<IndexIterator> getAllIterator(transaction::Methods* trx,
                                                ReadOwnWrites readOwnWrites);
  std::unique_ptr<IndexIterator> getAnyIterator(transaction::Methods* trx);

  /// @brief a method to skip certain documents in AQL write operations,
  /// this is only used in the Enterprise Edition for SmartGraphs
  virtual bool skipForAqlWrite(velocypack::Slice document,
                               std::string const& key) const;

  bool allowUserKeys() const noexcept;

  // SECTION: Modification Functions
  Result drop() override;
  Result rename(std::string&& name) override;

  // SECTION: Serialization
  void toVelocyPackIgnore(velocypack::Builder& result,
                          std::unordered_set<std::string> const& ignoreKeys,
                          Serialization context) const;

  velocypack::Builder toVelocyPackIgnore(
      std::unordered_set<std::string> const& ignoreKeys,
      Serialization context) const;

  void toVelocyPackForInventory(velocypack::Builder&) const;

  virtual void toVelocyPackForClusterInventory(velocypack::Builder&,
                                               bool useSystem, bool isReady,
                                               bool allInSync) const;

  using LogicalDataSource::properties;

  /// @brief updates properties of an existing DataSource
  virtual Result properties(velocypack::Slice definition);

  /// @brief return the figures for a collection
  virtual futures::Future<OperationResult> figures(
      bool details, OperationOptions const& options) const;

  /// @brief closes an open collection
  void close();

  // SECTION: Indexes

  /// @brief Create a new Index based on VelocyPack description
  virtual futures::Future<std::shared_ptr<Index>> createIndex(
      velocypack::Slice, bool&,
      std::shared_ptr<std::function<arangodb::Result(double)>> = nullptr,
      replication2::replicated_state::document::Replication2Callback
          replicationCb = nullptr);

  /// @brief Find index by definition
  std::shared_ptr<Index> lookupIndex(velocypack::Slice) const;

  /// @brief Find index by iid
  std::shared_ptr<Index> lookupIndex(IndexId) const;

  /// @brief Find index by name
  std::shared_ptr<Index> lookupIndex(std::string_view) const;

  Result dropIndex(IndexId iid);

  /// @brief processes a truncate operation
  Result truncate(transaction::Methods& trx, OperationOptions& options,
                  bool& usedRangeDelete);

  /// @brief compact-data operation
  void compact();

  /// @brief Persist the connected physical collection.
  ///        This should be called AFTER the collection is successfully
  ///        created and only on Sinlge/DBServer
  void persistPhysicalCollection();

  /// lock protecting the status and name
  basics::ReadWriteLock& statusLock() noexcept;

  /// @brief Defer a callback to be executed when the collection
  ///        can be dropped. The callback is supposed to drop
  ///        the collection and it is guaranteed that no one is using
  ///        it at that moment.
  void deferDropCollection(
      std::function<bool(LogicalCollection&)> const& callback);

  void computedValuesToVelocyPack(VPackBuilder&) const;

  // return a pointer to the computed values. can be a nullptr
  std::shared_ptr<ComputedValues> computedValues() const;

  void schemaToVelocyPack(VPackBuilder&) const;

  // return a pointer to the schema. can be a nullptr if no schema
  std::shared_ptr<ValidatorBase> schema() const;

  // validate a document on INSERT
  Result validate(std::shared_ptr<ValidatorBase> const& schema,
                  VPackSlice newDoc, VPackOptions const*) const;
  // validate a document on UPDATE/REPLACE
  Result validate(std::shared_ptr<ValidatorBase> const& schema,
                  VPackSlice modifiedDoc, VPackSlice oldDoc,
                  VPackOptions const*) const;

  // Get a reference to this KeyGenerator.
  KeyGenerator& keyGenerator() const noexcept { return *_keyGenerator; }

  transaction::CountCache& countCache() { return _countCache; }

  std::unique_ptr<FollowerInfo> const& followers() const;

  /// @brief returns the value of _syncByRevision
  bool syncByRevision() const noexcept;

  /// @brief returns the value of _syncByRevision, but only for "real"
  /// collections with data backing. returns false for all collections with no
  /// data backing.
  bool useSyncByRevision() const noexcept;

  /// @brief set the internal validator types. This should be handled with care
  /// and be set before the collection is persisted into the Agency.
  /// The value should not be modified at runtime.
  // (Technically no issue but will have side-effects on shards)
  void setInternalValidatorTypes(uint64_t type);

  uint64_t getInternalValidatorTypes() const noexcept;

  auto groupID() const noexcept
      -> arangodb::replication2::agency::CollectionGroupId;
  auto replicatedStateId() const noexcept -> arangodb::replication2::LogId;

 private:
  void initializeSmartAttributesBefore(velocypack::Slice info);
  void initializeSmartAttributesAfter(velocypack::Slice info);

  void prepareIndexes(velocypack::Slice indexesSlice);

  bool determineSyncByRevision() const;

  void decorateWithInternalValidators();

 protected:
  void addInternalValidator(std::unique_ptr<ValidatorBase>);

  Result appendVPack(velocypack::Builder& build, Serialization ctx,
                     bool safe) const override;

  Result updateSchema(VPackSlice schema);
  Result updateComputedValues(VPackSlice computedValues);

  void decorateWithInternalEEValidators();

  virtual void includeVelocyPackEnterprise(velocypack::Builder& result) const;

  // SECTION: Meta Information

  /// lock protecting the status and name
  mutable basics::ReadWriteLock _statusLock;

  /// @brief collection format version
  Version _version;

  // @brief Internal version used for caching
  uint32_t _v8CacheVersion;

  // @brief Collection type
  TRI_col_type_e const _type;

  /// @brief is this a global collection on a DBServer
  bool const _isAStub;

#ifdef USE_ENTERPRISE
  // @brief Flag if this collection is a disjoint smart one. (Enterprise Edition
  // only) can only be true if _isSmart is also true
  bool const _isDisjoint;
  // @brief Flag if this collection is a smart one. (Enterprise Edition only)
  bool const _isSmart;
  // @brief Flag if this collection is a child of a smart collection (Enterprise
  // Edition only)
  bool const _isSmartChild;
#endif

  bool const _allowUserKeys;

  bool _usesRevisionsAsDocumentIds;

  // SECTION: Properties
  std::atomic<bool> _waitForSync;

  std::atomic<bool> _syncByRevision;

#ifdef USE_ENTERPRISE
  mutable std::mutex
      _smartGraphAttributeLock;  // lock protecting the smartGraphAttribute
  std::string _smartGraphAttribute;

  std::string _smartJoinAttribute;
#endif

  transaction::CountCache _countCache;

  // options for key creation
  std::unique_ptr<KeyGenerator> _keyGenerator;

  std::unique_ptr<PhysicalCollection> _physical;

  mutable std::mutex _infoLock;  // lock protecting the info

  // the following contains in the cluster/DBserver case the information
  // which other servers are in sync with this shard. It is unset in all
  // other cases.
  std::unique_ptr<FollowerInfo> _followers;

  /// @brief sharding information
  std::unique_ptr<ShardingInfo> _sharding;

  // `_computedValues` must be used with atomic accessors only!!
  // We use acquire/release access (load/store) as we only care about atomicity.
  std::shared_ptr<ComputedValues> _computedValues;

  // `_schema` must be used with atomic accessors only!!
  // We use acquire/release access (load/store)
  std::shared_ptr<ValidatorBase> _schema;

  // This is a bitmap entry of InternalValidatorType entries.
  uint64_t _internalValidatorTypes;

  std::vector<std::unique_ptr<ValidatorBase>> _internalValidators;

  // Temporarily here, used for shards, only on DBServers
  std::optional<arangodb::replication2::LogId> _replicatedStateId;

  // TODO: Only quickly added
  std::optional<uint64_t> _groupId;
};

}  // namespace arangodb
