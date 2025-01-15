////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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

#pragma once

#include <mutex>
#include <unordered_set>

#include "Basics/Common.h"

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Cluster/ServerState.h"
#include "Containers/FlatHashMap.h"
#include "Containers/FlatHashSet.h"
#include "Metrics/Fwd.h"
#include "Scheduler/Scheduler.h"

namespace arangodb {
namespace application_features {
class CommunicationFeaturePhase;
class DatabaseFeaturePhase;
}  // namespace application_features
namespace metrics {
class MetricsFeature;
}
namespace network {
class ConnectionPool;
}

class AgencyCache;
class AgencyCallback;
class AgencyCallbackRegistry;
class ClusterInfo;
class DatabaseFeature;
class HeartbeatThread;

class ClusterFeature : public ArangodFeature {
 public:
  static constexpr std::string_view name() noexcept { return "Cluster"; }

  explicit ClusterFeature(Server& server);
  ~ClusterFeature();

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override final;
  void start() override final;
  void stop() override final;
  void beginShutdown() override final;
  void unprepare() override final;

  void allocateMembers();
  void shutdown();

  std::vector<std::string> agencyEndpoints() const { return _agencyEndpoints; }

  std::string agencyPrefix() const { return _agencyPrefix; }

  AgencyCache& agencyCache();

  /// @return role argument as it was supplied by a user
  std::string const& myRole() const noexcept { return _myRole; }

  void notify();

  AgencyCallbackRegistry* agencyCallbackRegistry() const {
    return _agencyCallbackRegistry.get();
  }

  std::string const agencyCallbacksPath() const {
    return "/_api/agency/agency-callbacks";
  }

  std::string const clusterRestPath() const { return "/_api/cluster"; }

  void setUnregisterOnShutdown(bool);
  bool createWaitsForSyncReplication() const {
    return _createWaitsForSyncReplication;
  }
  std::uint32_t writeConcern() const { return _writeConcern; }
  std::uint32_t systemReplicationFactor() const {
    return _systemReplicationFactor;
  }
  std::uint32_t defaultReplicationFactor() const {
    return _defaultReplicationFactor;
  }
  std::uint32_t maxNumberOfShards() const { return _maxNumberOfShards; }
  std::uint32_t minReplicationFactor() const { return _minReplicationFactor; }
  std::uint32_t maxReplicationFactor() const { return _maxReplicationFactor; }
  std::uint32_t maxNumberOfMoveShards() const { return _maxNumberOfMoveShards; }
  bool forceOneShard() const { return _forceOneShard; }
  /// @brief index creation timeout in seconds. note: this used to be
  /// a configurable parameter in previous versions, but is now hard-coded.
  double indexCreationTimeout() const { return _indexCreationTimeout; }

  std::shared_ptr<HeartbeatThread> heartbeatThread();

  ClusterInfo& clusterInfo();

  /// @brief permissions required to access /_admin/cluster REST API endpoint:
  /// - "jwt-all"    = JWT required to access all operations
  /// - "jwt-write"  = JWT required to access post/put/delete operations
  /// - "jwt-compat" = compatibility mode = same permissions as in 3.7
  std::string const& apiJwtPolicy() const noexcept { return _apiJwtPolicy; }

  std::uint32_t statusCodeFailedWriteConcern() const {
    return _statusCodeFailedWriteConcern;
  }

  metrics::Counter& followersDroppedCounter() {
    TRI_ASSERT(_followersDroppedCounter != nullptr);
    return *_followersDroppedCounter;
  }
  metrics::Counter& followersRefusedCounter() {
    TRI_ASSERT(_followersRefusedCounter != nullptr);
    return *_followersRefusedCounter;
  }
  metrics::Counter& followersWrongChecksumCounter() {
    TRI_ASSERT(_followersWrongChecksumCounter != nullptr);
    return *_followersWrongChecksumCounter;
  }
  metrics::Counter& syncTreeRebuildCounter() {
    TRI_ASSERT(_syncTreeRebuildCounter != nullptr);
    return *_syncTreeRebuildCounter;
  }
  metrics::Counter& potentiallyDirtyDocumentReadsCounter() {
    TRI_ASSERT(_potentiallyDirtyDocumentReadsCounter != nullptr);
    return *_potentiallyDirtyDocumentReadsCounter;
  }
  metrics::Counter& dirtyReadQueriesCounter() {
    TRI_ASSERT(_dirtyReadQueriesCounter != nullptr);
    return *_dirtyReadQueriesCounter;
  }

  /**
   * @brief Add databases to dirty list
   */
  void addDirty(std::string const& database);
  void addDirty(containers::FlatHashSet<std::string> const& databases,
                bool callNotify);
  void addDirty(
      containers::FlatHashMap<std::string, std::shared_ptr<VPackBuilder>> const&
          changeset);
  std::unordered_set<std::string> allDatabases() const;

  /**
   * @brief Swap out the list of dirty databases
   *        This method must not be called by any other mechanism than
   *        the very start of a single maintenance run.
   */
  containers::FlatHashSet<std::string> dirty();

  /**
   * @brief Check database for dirtyness
   */
  bool isDirty(std::string const& database) const;

  /// @brief hand out async agency comm connection pool pruning:
  void pruneAsyncAgencyConnectionPool();

  /// the following methods may also be called from tests

  void shutdownHeartbeatThread();
  /// @brief wait for the AgencyCache to shut down
  /// note: this may be called multiple times during shutdown
  void shutdownAgencyCache();
  /// @brief wait for the Plan and Current syncer to shut down
  /// note: this may be called multiple times during shutdown
  void waitForSyncersToStop();

#ifdef ARANGODB_USE_GOOGLE_TESTS
  void setSyncerShutdownCode(ErrorCode code) { _syncerShutdownCode = code; }
#endif

  metrics::Histogram<metrics::LogScale<uint64_t>>&
  agency_comm_request_time_ms() {
    return _agency_comm_request_time_ms;
  }

 protected:
  void startHeartbeatThread(AgencyCallbackRegistry* agencyCallbackRegistry,
                            uint64_t interval_ms,
                            uint64_t maxFailsBeforeWarning,
                            std::string const& endpoints);

 private:
  ClusterFeature(Server& server, metrics::MetricsFeature& metrics,
                 DatabaseFeature& database, size_t registration);
  void reportRole(ServerState::RoleEnum);
  void scheduleConnectivityCheck(std::uint32_t inSeconds);
  void runConnectivityCheck();

  std::vector<std::string> _agencyEndpoints;
  std::string _agencyPrefix;
  std::string _myRole;
  std::string _myEndpoint;
  std::string _myAdvertisedEndpoint;
  std::string _apiJwtPolicy;
  std::uint32_t _connectivityCheckInterval = 3600;  // seconds
  std::uint32_t _writeConcern = 1;                  // write concern
  std::uint32_t _defaultReplicationFactor =
      0;  // a value of 0 means it will use the min replication factor
  std::uint32_t _systemReplicationFactor = 2;
  std::uint32_t _minReplicationFactor =
      1;  // minimum replication factor (0 = unrestricted)
  std::uint32_t _maxReplicationFactor =
      10;  // maximum replication factor (0 = unrestricted)
  std::uint32_t _maxNumberOfShards =
      1000;  // maximum number of shards (0 = unrestricted)
  std::uint32_t _maxNumberOfMoveShards =
      10;  // maximum number of shards to be moved per rebalance operation
           // if value = 0, no move shards operations will be scheduled
  ErrorCode _syncerShutdownCode = TRI_ERROR_SHUTTING_DOWN;
  bool _createWaitsForSyncReplication = true;
  bool _forceOneShard = false;
  bool _unregisterOnShutdown = false;
  bool _enableCluster = false;
  bool _requirePersistedId = false;
  // The following value indicates what HTTP status code should be returned if
  // a configured write concern cannot currently be fulfilled. The old
  // behavior (currently the default) means that a 403 Forbidden
  // with an error of 1004 ERROR_ARANGO_READ_ONLY is returned. It is possible to
  // adjust the behavior so that an HTTP 503 Service Unavailable with an error
  // of 1429 ERROR_REPLICATION_WRITE_CONCERN_NOT_FULFILLED is returned.
  uint32_t _statusCodeFailedWriteConcern = 403;
  /// @brief coordinator timeout for index creation. defaults to 4 days
  double _indexCreationTimeout = 72.0 * 3600.0;
  std::unique_ptr<ClusterInfo> _clusterInfo;
  std::shared_ptr<HeartbeatThread> _heartbeatThread;
  std::unique_ptr<AgencyCache> _agencyCache;
  uint64_t _heartbeatInterval = 0;
  std::unique_ptr<AgencyCallbackRegistry> _agencyCallbackRegistry;
  metrics::MetricsFeature& _metrics;
  ServerState::RoleEnum _requestedRole = ServerState::RoleEnum::ROLE_UNDEFINED;
  metrics::Histogram<metrics::LogScale<uint64_t>>& _agency_comm_request_time_ms;
  std::unique_ptr<network::ConnectionPool> _asyncAgencyCommPool;
  metrics::Counter* _followersDroppedCounter = nullptr;
  metrics::Counter* _followersRefusedCounter = nullptr;
  metrics::Counter* _followersWrongChecksumCounter = nullptr;
  // note: this metric is only there for downwards-compatibility reasons. it
  // will always have a value of 0.
  metrics::Counter* _followersTotalRebuildCounter = nullptr;
  metrics::Counter* _syncTreeRebuildCounter = nullptr;
  metrics::Counter* _potentiallyDirtyDocumentReadsCounter = nullptr;
  metrics::Counter* _dirtyReadQueriesCounter = nullptr;
  std::shared_ptr<AgencyCallback> _hotbackupRestoreCallback = nullptr;

  /// @brief lock for dirty database list
  mutable std::mutex _dirtyLock;
  /// @brief dirty databases, where a job could not be posted)
  containers::FlatHashSet<std::string> _dirtyDatabases;

  std::mutex _connectivityCheckMutex;
  Scheduler::WorkHandle _connectivityCheck;
  metrics::Counter* _connectivityCheckFailsCoordinators = nullptr;
  metrics::Counter* _connectivityCheckFailsDBServers = nullptr;
};

}  // namespace arangodb
