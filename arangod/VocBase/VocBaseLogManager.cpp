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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "VocBaseLogManager.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Result.tpp"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ServerState.h"
#include "Logger/LogContextKeys.h"
#include "Logger/Logger.h"
#include "Metrics/Counter.h"
#include "Metrics/Gauge.h"
#include "Network/NetworkFeature.h"
#include "Replication2/LoggerContext.h"
#include "Replication2/ReplicatedLog/DefaultRebootIdCache.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/NetworkAttachedFollower.h"
#include "Replication2/ReplicatedLog/ReplicatedLogFeature.h"
#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Replication2/ReplicatedState/ReplicatedStateFeature.h"
#include "Replication2/Storage/IStorageEngineMethods.h"
#include "RestServer/arangod.h"
#include "Scheduler/SchedulerFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "vocbase.h"
#include <cinttypes>

using namespace arangodb;

VocBaseLogManager::VocBaseLogManager(TRI_vocbase_t& vocbase,
                                     DatabaseID database)
    : _server(vocbase.server()),
      _vocbase(vocbase),
      _logContext(
          LoggerContext{Logger::REPLICATION2}.with<logContextKeyDatabaseName>(
              std::move(database))) {}

auto VocBaseLogManager::getReplicatedStateById(replication2::LogId id)
    -> ResultT<
        std::shared_ptr<replication2::replicated_state::ReplicatedStateBase>> {
  auto guard = _guardedData.getLockedGuard();
  if (auto iter = guard->statesAndLogs.find(id);
      iter != guard->statesAndLogs.end()) {
    return {iter->second.state};
  } else {
    return Result(TRI_ERROR_REPLICATION_REPLICATED_STATE_NOT_FOUND,
                  fmt::format("replicated state {} not found", id.id()));
  }
}

void VocBaseLogManager::registerReplicatedState(
    replication2::LogId id,
    std::unique_ptr<arangodb::replication2::storage::IStorageEngineMethods>
        methods) {
  auto meta = methods->readMetadata();
  if (meta.fail()) {
    THROW_ARANGO_EXCEPTION(meta.result());
  }

  auto& feature = _server.getFeature<
      replication2::replicated_state::ReplicatedStateAppFeature>();

  auto result = _guardedData.getLockedGuard()->buildReplicatedStateWithMethods(
      id, meta->specification.type, meta->specification.parameters->slice(),
      feature, _logContext.withTopic(Logger::REPLICATED_STATE), _server,
      _vocbase, std::move(methods));
  if (result.fail()) {
    THROW_ARANGO_EXCEPTION(result.result());
  }
}

void VocBaseLogManager::resignAll() noexcept {
  auto guard = _guardedData.getLockedGuard();
  guard->resignAllWasCalled = true;
  for (auto&& [id, val] : guard->statesAndLogs) {
    auto&& log = val.log;
    auto storage = std::move(*log).resign();
    storage.reset();
  }
  guard->statesAndLogs.clear();
}

auto VocBaseLogManager::updateReplicatedState(
    arangodb::replication2::LogId id,
    replication2::agency::LogPlanTermSpecification const& term,
    replication2::agency::ParticipantsConfig const& config) -> Result {
  auto guard = _guardedData.getLockedGuard();
  auto myself = replication2::agency::ServerInstanceReference(
      ServerState::instance()->getId(), ServerState::instance()->getRebootId());
  if (auto iter = guard->statesAndLogs.find(id);
      iter != guard->statesAndLogs.end()) {
    iter->second.log->updateConfig(term, config, std::move(myself))
        .thenFinal([&voc = _vocbase](auto&&) {
          voc.server().getFeature<ClusterFeature>().addDirty(voc.name());
        });
    return {};
  } else {
    return Result::fmt(TRI_ERROR_REPLICATION_REPLICATED_LOG_NOT_FOUND, id);
  }
}

void VocBaseLogManager::prepareDropAll() noexcept {
  std::map<arangodb::replication2::LogId, GuardedData::StateAndLog>
      statesAndLogs;
  {
    // We steal all logs from the _guardedData.
    // We need to give up the guarded Log, and we also
    // need to have the logs to be unreachable from now on.
    auto guard = _guardedData.getLockedGuard();
    guard->resignAllWasCalled = true;
    statesAndLogs.swap(guard->statesAndLogs);
    TRI_ASSERT(guard->statesAndLogs.empty());
  }
  for (auto&& [id, val] : statesAndLogs) {
    if (auto res = basics::catchVoidToResult([&, &val = val]() {
          auto storage = resignAndDrop(val);
          storage.reset();
        });
        res.fail()) {
      LOG_CTX("1d158", WARN, _logContext)
          << "Failure to drop replicated log will be ignored, as all "
             "replication resources in "
          << _vocbase.name() << " are being dropped " << id;
    }
  }
}

auto VocBaseLogManager::dropReplicatedState(arangodb::replication2::LogId id)
    -> arangodb::Result {
  LOG_CTX("658c6", DEBUG, _logContext) << "Dropping replicated state " << id;

  auto stateAndLog = _guardedData.getLockedGuard()->stealReplicatedState(id);
  if (stateAndLog.fail()) {
    return stateAndLog.result();
  }
  auto resignRes = basics::catchToResultT(
      [&]() -> std::unique_ptr<replication2::storage::IStorageEngineMethods> {
        return resignAndDrop(stateAndLog.get());
      });
  if (resignRes.fail()) {
    LOG_CTX("18db5", ERR, _logContext)
        << "failed to drop replicated log " << resignRes.result();
    return resignRes.result();
  }
  // Now we may delete the persistent metadata.
  auto storage = std::move(*resignRes);
  StorageEngine& engine = _vocbase.engine();
  auto res = engine.dropReplicatedState(_vocbase, storage);

  if (res.fail()) {
    TRI_ASSERT(storage != nullptr);
    LOG_CTX("998cc", ERR, _logContext)
        << "failed to drop replicated log " << res.errorMessage();
    return res;
  }
  TRI_ASSERT(storage == nullptr);

  auto& feature = _server.getFeature<ReplicatedLogFeature>();
  feature.metrics()->replicatedLogDeletionNumber->count();

  return {};
}

auto VocBaseLogManager::resignAndDrop(GuardedData::StateAndLog& stateAndLog)
    -> std::unique_ptr<replication2::storage::IStorageEngineMethods> {
  auto& state = stateAndLog.state;
  auto& log = stateAndLog.log;

  // Get the state handle so we can drop the state later
  auto stateHandle = log->disconnect(std::move(stateAndLog.connection));

  // resign the log now, before we update the metadata to avoid races
  // on the storage.
  auto storage = std::move(*log).resign();

  auto metadata = storage->readMetadata();
  if (metadata.fail()) {
    THROW_ARANGO_EXCEPTION(
        std::move(metadata).result());  // state untouched after this
  }

  // Invalidate the snapshot in persistent storage.
  metadata->snapshot.updateStatus(
      replication2::replicated_state::SnapshotStatus::kInvalidated);
  // TODO make sure other methods working on the state, probably meaning
  //      configuration updates, handle an invalidated snapshot correctly.
  if (auto res = storage->updateMetadata(*metadata); res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  // Drop the replicated state. This will also remove its associated
  // resources, e.g. the shard/collection will be dropped.
  // This must happen only after the snapshot is persistently marked as
  // failed.
  std::move(*state).drop(std::move(stateHandle));

  return storage;
}

auto VocBaseLogManager::getReplicatedLogsStatusMap() const
    -> std::unordered_map<arangodb::replication2::LogId,
                          arangodb::replication2::maintenance::LogStatus> {
  std::unordered_map<arangodb::replication2::LogId,
                     arangodb::replication2::maintenance::LogStatus>
      result;
  auto guard = _guardedData.getLockedGuard();
  for (auto& [id, value] : guard->statesAndLogs) {
    result.emplace(id, value.log->getMaintenanceLogStatus());
  }
  return result;
}

auto VocBaseLogManager::getReplicatedStatesStatus() const
    -> std::unordered_map<arangodb::replication2::LogId,
                          arangodb::replication2::replicated_log::LogStatus> {
  std::unordered_map<arangodb::replication2::LogId,
                     arangodb::replication2::replicated_log::LogStatus>
      result;
  auto guard = _guardedData.getLockedGuard();
  for (auto& [id, value] : guard->statesAndLogs) {
    result.emplace(id, value.log->getStatus());
  }
  return result;
}

auto VocBaseLogManager::createReplicatedState(replication2::LogId id,
                                              std::string_view type,
                                              VPackSlice parameter)
    -> ResultT<
        std::shared_ptr<replication2::replicated_state::ReplicatedStateBase>> {
  auto& feature = _server.getFeature<
      replication2::replicated_state::ReplicatedStateAppFeature>();
  return _guardedData.doUnderLock(
      [&](GuardedData& data)
          -> ResultT<std::shared_ptr<
              replication2::replicated_state::ReplicatedStateBase>> {
        if (_vocbase.isDropped()) {
          // Note that this check must happen under the _guardedData mutex, so
          // there's no race between a create call, and resignAll, which
          // happens after markAsDropped().
          return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
        }
        auto state = data.buildReplicatedState(
            id, type, parameter, feature,
            _logContext.withTopic(Logger::REPLICATED_STATE), _server, _vocbase);
        LOG_CTX("2bf8d", DEBUG, _logContext)
            << "Created replicated state " << id << " impl = " << type
            << " result = " << state.errorNumber();

        return state;
      });
}

auto VocBaseLogManager::GuardedData::buildReplicatedState(
    replication2::LogId const id, std::string_view type, VPackSlice parameters,
    replication2::replicated_state::ReplicatedStateAppFeature& feature,
    LoggerContext const& logContext, ArangodServer& server,
    TRI_vocbase_t& vocbase)
    -> ResultT<
        std::shared_ptr<replication2::replicated_state::ReplicatedStateBase>> {
  using namespace arangodb::replication2;
  using namespace arangodb::replication2::replicated_state;
  StorageEngine& engine = vocbase.engine();

  {
    VPackBufferUInt8 buffer;
    buffer.append(parameters.start(), parameters.byteSize());
    auto parametersCopy = velocypack::SharedSlice(std::move(buffer));

    auto metadata = storage::PersistedStateInfo{
        .stateId = id,
        .snapshot = {.status = replicated_state::SnapshotStatus::kCompleted,
                     .timestamp = {},
                     .error = {}},
        .generation = {},
        .specification = {.type = std::string(type),
                          .parameters = std::move(parametersCopy)},
        .stateOwnedMetadata = feature.getDefaultStateOwnedMetadata(type)};
    auto maybeStorage = engine.createReplicatedState(vocbase, id, metadata);

    if (maybeStorage.fail()) {
      return std::move(maybeStorage).result();
    }

    return buildReplicatedStateWithMethods(id, type, parameters, feature,
                                           logContext, server, vocbase,
                                           std::move(*maybeStorage));
  }
}

auto VocBaseLogManager::GuardedData::buildReplicatedStateWithMethods(
    replication2::LogId const id, std::string_view type, VPackSlice parameters,
    replication2::replicated_state::ReplicatedStateAppFeature& feature,
    LoggerContext const& logContext, ArangodServer& server,
    TRI_vocbase_t& vocbase,
    std::unique_ptr<arangodb::replication2::storage::IStorageEngineMethods>
        storage)
    -> ResultT<std::shared_ptr<
        replication2::replicated_state::ReplicatedStateBase>> try {
  using namespace arangodb::replication2;
  using namespace arangodb::replication2::replicated_state;
  // TODO Make this atomic without crashing on errors if possible

  if (resignAllWasCalled) {
    // TODO This error code is not always correct. We'd probably have to
    // distinguish between whether resignAll was called due to a shutdown, or
    // due to dropping a database.
    return Result{
        TRI_ERROR_SHUTTING_DOWN,
        fmt::format("Abort replicated state creation because all logs from the "
                    "current database are being resigned, log id: {}",
                    id.id())};
  }
  if (auto iter = statesAndLogs.find(id); iter != std::end(statesAndLogs)) {
    return {TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER};
  }

  // 1. Allocate all memory
  //    1.1 allocate node in map.
  //    1.2 allocate state (and log)
  //    1.2 allocate state (and log)
  // 2. write to storage engine
  // ---- from now on no errors are allowed
  // 3. forward storage interface to replicated state
  // 4. start up with initial configuration

  LOG_CTX("ef73d", DEBUG, logContext)
      << "building new replicated state " << id << " impl = " << type;

  // prepare map
  auto stateAndLog = StateAndLog{};

  struct NetworkFollowerFactory
      : replication2::replicated_log::IAbstractFollowerFactory {
    NetworkFollowerFactory(
        TRI_vocbase_t& vocbase, replication2::LogId id,
        std::shared_ptr<replication2::ReplicatedLogGlobalSettings const>
            options)
        : vocbase(vocbase), id(id), options(std::move(options)) {}
    auto constructFollower(const ParticipantId& participantId)
        -> std::shared_ptr<
            replication2::replicated_log::AbstractFollower> override {
      auto* pool = vocbase.server().getFeature<NetworkFeature>().pool();

      return std::make_shared<
          replication2::replicated_log::NetworkAttachedFollower>(
          pool, participantId, vocbase.name(), id, options);
    }

    auto constructLeaderCommunicator(const ParticipantId& participantId)
        -> std::shared_ptr<replicated_log::ILeaderCommunicator> override {
      auto* pool = vocbase.server().getFeature<NetworkFeature>().pool();

      return std::make_shared<
          replication2::replicated_log::NetworkLeaderCommunicator>(
          pool, participantId, vocbase.name(), id);
    }

    TRI_vocbase_t& vocbase;
    replication2::LogId id;
    std::shared_ptr<replication2::ReplicatedLogGlobalSettings const> options;
  };

  auto myself = replication2::agency::ServerInstanceReference(
      ServerState::instance()->getId(), ServerState::instance()->getRebootId());

  struct MyScheduler : IScheduler {
    auto delayedFuture(std::chrono::steady_clock::duration duration,
                       std::string_view name)
        -> futures::Future<futures::Unit> override {
      if (name.data() == nullptr) {
        name = "replication-2";
      }
      return SchedulerFeature::SCHEDULER->delay(name, duration);
    }

    auto queueDelayed(
        std::string_view name, std::chrono::steady_clock::duration delay,
        fu2::unique_function<void(bool canceled)> handler) noexcept
        -> WorkItemHandle override {
      auto handle = SchedulerFeature::SCHEDULER->queueDelayed(
          name, RequestLane::CLUSTER_INTERNAL, delay, std::move(handler));
      struct MyWorkItem : WorkItem {
        explicit MyWorkItem(Scheduler::WorkHandle handle) : handle(handle) {}
        Scheduler::WorkHandle handle;
      };
      return std::make_shared<MyWorkItem>(std::move(handle));
    }

    void queue(fu2::unique_function<void()> cb) noexcept override {
      SchedulerFeature::SCHEDULER->queue(RequestLane::CLUSTER_INTERNAL,
                                         std::move(cb));
    }
  };

  auto maybeMetadata = storage->readMetadata();
  if (!maybeMetadata) {
    throw basics::Exception(std::move(maybeMetadata).result());
  }

  auto sched = std::make_shared<MyScheduler>();
  auto& logFeature = server.getFeature<ReplicatedLogFeature>();
  auto& ci = server.getFeature<ClusterFeature>().clusterInfo();
  auto& log = stateAndLog.log = std::invoke([&]() {
    return std::make_shared<
        arangodb::replication2::replicated_log::ReplicatedLog>(
        std::move(storage), logFeature.metrics(), logFeature.options(),
        std::make_shared<replicated_log::DefaultParticipantsFactory>(
            std::make_shared<NetworkFollowerFactory>(vocbase, id,
                                                     logFeature.options()),
            sched, std::make_shared<replicated_log::DefaultRebootIdCache>(ci)),
        logContext, myself);
  });

  auto& state = stateAndLog.state = feature.createReplicatedState(
      type, vocbase.name(), id, log, logContext, sched);

  auto const& stateParams = maybeMetadata->specification.parameters;
  auto&& stateHandle = state->createStateHandle(vocbase, stateParams);

  stateAndLog.connection = log->connect(std::move(stateHandle));

  auto emplaceResult = statesAndLogs.emplace(id, std::move(stateAndLog));
  auto [iter, inserted] = emplaceResult;
  ADB_PROD_ASSERT(inserted);
  auto metrics = logFeature.metrics();
  metrics->replicatedLogNumber->fetch_add(1);
  metrics->replicatedLogCreationNumber->count();

  return iter->second.state;
} catch (basics::Exception& ex) {
  // If we created the state on-disk, but failed to add it to the map, we
  // cannot continue safely.
  LOG_TOPIC("35db0", FATAL, Logger::REPLICATION2)
      << "Failed to create replicated state: " << ex.message() << ", thrown at "
      << ex.location();
  std::abort();
} catch (std::exception& ex) {
  // If we created the state on-disk, but failed to add it to the map, we
  // cannot continue safely.
  LOG_TOPIC("35daf", FATAL, Logger::REPLICATION2)
      << "Failed to create replicated state: " << ex.what();
  std::abort();
} catch (...) {
  std::abort();
}

auto VocBaseLogManager::GuardedData::stealReplicatedState(
    replication2::LogId id) -> ResultT<GuardedData::StateAndLog> {
  if (auto iter = statesAndLogs.find(id); iter != statesAndLogs.end()) {
    auto stateAndLog = std::move(iter->second);
    statesAndLogs.erase(iter);
    return stateAndLog;
  } else {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }
}
