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
/// @author Dr. Frank Celler
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "DatabaseFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryCache.h"
#include "Aql/QueryList.h"
#include "Aql/QueryRegistry.h"
#include "Auth/UserManager.h"
#include "Basics/ArangoGlobalContext.h"
#include "Basics/FileUtils.h"
#include "Basics/NumberUtils.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/application-exit.h"
#include "Basics/files.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Metrics/MetricsFeature.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "Replication/ReplicationClients.h"
#include "Replication/ReplicationFeature.h"
#include "Replication2/Version.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FileDescriptorsFeature.h"
#include "RestServer/IOHeartbeatThread.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Scheduler/SchedulerFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/OperationOrigin.h"
#include "Utilities/NameValidator.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/CursorRepository.h"
#include "Utils/Events.h"
#ifdef USE_V8
#include "V8Server/V8DealerFeature.h"
#endif
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>

using namespace arangodb::options;

namespace arangodb {
namespace {

template<typename T>
void waitUnique(std::shared_ptr<T> const& ptr) {
  // It's safe because we guarantee we have not weak_ptr
  while (ptr.use_count() != 1) {
    // 250ms from old DataProtector code authored by Max Neunhoeffer
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
  std::atomic_thread_fence(std::memory_order_acquire);
}

CreateDatabaseInfo createExpressionVocbaseInfo(ArangodServer& server) {
  CreateDatabaseInfo info{server, ExecContext::current()};
  // name does not matter. We just need validity check to pass.
  auto r = info.load("Z", std::numeric_limits<uint64_t>::max());
  TRI_ASSERT(r.ok());
  return info;
}

/// @brief sandbox vocbase for executing calculation queries.
/// TODO: instead of global variable, this should be an instance variable in the
/// DatabaseFeature. otherwise it is problematic to create two individual
/// DatabaseFeature objects with overlapping lifetime, as we do in the unit
/// tests. by creating multiple DatabaseFeature objects, they may each clobber
/// the global calculationVocbase object.
/// the main user of the calculationVocbase is the IResearchAqlAnalyzer.
/// its constructor should be changed so that instead of referring to global
/// static methods to access the calculationVocbase, the DatabaseFeature
/// instance should be passed into it.
std::unique_ptr<TRI_vocbase_t> calculationVocbase;
}  // namespace

DatabaseManagerThread::DatabaseManagerThread(Server& server,
                                             DatabaseFeature& databaseFeature,
                                             StorageEngine& engine)
    : ServerThread<ArangodServer>(server, "DatabaseManager"),
      _databaseFeature(databaseFeature),
      _engine(engine)
#ifdef USE_V8
      ,
      _dealer(server.getFeature<V8DealerFeature>())
#endif
{
}

DatabaseManagerThread::~DatabaseManagerThread() { shutdown(); }

void DatabaseManagerThread::run() {
  int cleanupCycles = 0;

  while (true) {
    try {
      // if we find a database to delete, it will be automatically deleted at
      // the end of this scope
      std::unique_ptr<TRI_vocbase_t> database;
      {
        std::lock_guard lock{_databaseFeature._databasesMutex};
        auto& dropped = _databaseFeature._droppedDatabases;
        // check if we have to drop some database
        for (auto it = dropped.begin(), end = dropped.end(); it != end; ++it) {
          TRI_ASSERT(*it != nullptr);
          if ((*it)->isDangling()) {
            // found a database to delete
            // now move the to-be-deleted database from _droppedDatabases
            // into the unique_ptr
            auto* p = *it;
            dropped.erase(it);
            database.reset(p);
            break;
          }
        }
      }

      if (database != nullptr) {
        TRI_ASSERT(!database->isSystem());
        if (ServerState::instance()->isCoordinator()) {
          continue;  // TODO(MBkkt) Why do we have this thread on Coordinator?
        }

        auto r = basics::catchVoidToResult([&] { database->shutdown(); });
        if (!r.ok()) {
          LOG_TOPIC("b3db4", ERR, Logger::FIXME)
              << "failed to shutdown database '" << database->name()
              << "': " << r.errorMessage();
        }

        iresearch::cleanupDatabase(*database);

        auto* queryRegistry = QueryRegistryFeature::registry();
#ifdef USE_V8
        if (_dealer.isEnabled() || queryRegistry != nullptr) {
#else
        if (queryRegistry != nullptr) {
#endif
          // TODO(MBkkt) Why shouldn't we remove database data
          //  if exists database with same name?
          std::lock_guard lockCreate{_databaseFeature._databaseCreateLock};
          // there is single thread which removes databases, so it's safe
          auto* same = _databaseFeature.lookupDatabase(database->name());
          TRI_ASSERT(same == nullptr || same->id() != database->id());
          if (same == nullptr) {
#ifdef USE_V8
            if (_dealer.isEnabled()) {
              _dealer.cleanupDatabase(*database);
            }
#endif
            if (queryRegistry != nullptr) {
              queryRegistry->destroy(database->name());
            }
          }
        }

        try {
          r = _engine.dropDatabase(*database);
          if (!r.ok()) {
            LOG_TOPIC("fb244", ERR, Logger::FIXME)
                << "dropping database '" << database->name()
                << "' failed: " << r.errorMessage();
          }
        } catch (std::exception const& ex) {
          LOG_TOPIC("d30a2", ERR, Logger::FIXME)
              << "dropping database '" << database->name()
              << "' failed: " << ex.what();
        } catch (...) {
          LOG_TOPIC("0a30c", ERR, Logger::FIXME)
              << "dropping database '" << database->name() << "' failed";
        }
      } else {
        // perform some cleanup tasks
        if (isStopping()) {
          // done
          break;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(waitTime()));

        // The following is only necessary after a wait:
        // perform cursor cleanup here
        if (++cleanupCycles >= 10) {
          cleanupCycles = 0;

          auto databases = _databaseFeature._databases.load();

          bool force = isStopping();
          for (auto& p : *databases) {
            auto* vocbase = p.second;
            TRI_ASSERT(vocbase != nullptr);

            try {
              vocbase->cursorRepository()->garbageCollect(force);
            } catch (...) {
            }
            double const now = []() {
              using namespace std::chrono;
              return duration<double>(steady_clock::now().time_since_epoch())
                  .count();
            }();
            vocbase->replicationClients().garbageCollect(now);
          }
        }

        // unfortunately the FileDescriptorsFeature can only be used
        // if the following ifdef applies
#ifdef TRI_HAVE_GETRLIMIT
        // update metric for the number of open file descriptors.
        // technically this does not belong here, but there is no other
        // ideal place for this
        FileDescriptorsFeature& fds =
            server().getFeature<FileDescriptorsFeature>();
        fds.countOpenFilesIfNeeded();
#endif
      }
    } catch (...) {
    }
  }
}

DatabaseFeature::DatabaseFeature(Server& server)
    : ArangodFeature{server, *this} {
  setOptional(false);
  startsAfter<BasicFeaturePhaseServer>();

  startsAfter<AuthenticationFeature>();
  startsAfter<CacheManagerFeature>();
  startsAfter<EngineSelectorFeature>();
  startsAfter<InitDatabaseFeature>();
  startsAfter<StorageEngineFeature>();
  startsAfter<metrics::MetricsFeature>();
}

DatabaseFeature::~DatabaseFeature() = default;

void DatabaseFeature::collectOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  options->addSection("database", "database options");

  auto static const allowedReplicationVersions = [] {
    auto result = std::unordered_set<std::string>();
    for (auto const& version : replication::allowedVersions) {
      result.emplace(replication::versionToString(version));
    }
    return result;
  }();

  options
      ->addOption("--database.default-replication-version",
                  "The default replication version, can be overwritten "
                  "when creating a new database, possible values: 1, 2",
                  new DiscreteValuesParameter<StringParameter>(
                      &_defaultReplicationVersion, allowedReplicationVersions),
                  options::makeDefaultFlags(options::Flags::Uncommon,
                                            options::Flags::Experimental))
      .setIntroducedIn(31200);

  options->addOption(
      "--database.wait-for-sync",
      "The default waitForSync behavior. Can be overwritten when creating a "
      "collection.",
      new options::BooleanParameter(&_defaultWaitForSync),
      options::makeDefaultFlags(options::Flags::Uncommon));

  // the following option was obsoleted in 3.9
  options->addObsoleteOption(
      "--database.force-sync-properties",
      "Force syncing of collection properties to disk after creating a "
      "collection or updating its properties. Otherwise, let the waitForSync "
      "property of each collection determine it.",
      false);

  options->addOption("--database.ignore-datafile-errors",
                     "Load collections even if datafiles may contain errors.",
                     new options::BooleanParameter(&_ignoreDatafileErrors),
                     options::makeDefaultFlags(options::Flags::Uncommon));

  options
      ->addOption("--database.extended-names",
                  "Allow most UTF-8 characters in the names of databases, "
                  "collections, Views, and indexes. Once in use, "
                  "this option cannot be turned off again.",
                  new options::BooleanParameter(&_extendedNames),
                  options::makeDefaultFlags(options::Flags::Uncommon,
                                            options::Flags::Experimental))
      .setIntroducedIn(30900);

  options->addOldOption("database.extended-names-databases",
                        "database.extended-names");

  options
      ->addOption("--database.io-heartbeat",
                  "Perform I/O heartbeat to test the underlying volume.",
                  new options::BooleanParameter(&_performIOHeartbeat),
                  options::makeDefaultFlags(options::Flags::Uncommon))
      .setIntroducedIn(30807)
      .setIntroducedIn(30902);

  options
      ->addOption("--database.max-databases",
                  "The maximum number of databases that can exist in parallel.",
                  new options::SizeTParameter(&_maxDatabases))
      .setLongDescription(R"(If the maximum number of databases is reached, no
additional databases can be created in the deployment. In order to create additional
databases, other databases need to be removed first.")")
      .setIntroducedIn(31200);

  // the following option was obsoleted in 3.9
  options->addObsoleteOption(
      "--database.old-system-collections",
      "Create and use deprecated system collection (_modules, _fishbowl).",
      false);

  // the following option was obsoleted in 3.8
  options->addObsoleteOption(
      "--database.throw-collection-not-loaded-error",
      "throw an error when accessing a collection that is still loading",
      false);

  // the following option was removed in 3.7
  options->addObsoleteOption(
      "--database.maximal-journal-size",
      "default maximal journal size, can be overwritten when "
      "creating a collection",
      true);

  // the following option was removed in 3.2
  options->addObsoleteOption(
      "--database.index-threads",
      "threads to start for parallel background index creation", true);

  // the following hidden option was removed in 3.4
  options->addObsoleteOption(
      "--database.check-30-revisions",
      "check for revision values from ArangoDB 3.0 databases", true);

  // the following options were removed in 3.2
  options->addObsoleteOption(
      "--database.revision-cache-chunk-size",
      "chunk size (in bytes) for the document revisions cache", true);
  options->addObsoleteOption(
      "--database.revision-cache-target-size",
      "total target size (in bytes) for the document revisions cache", true);
}

void DatabaseFeature::validateOptions(
    std::shared_ptr<options::ProgramOptions> options) {
  // check the misuse of startup options
  if (_checkVersion && _upgrade) {
    LOG_TOPIC("a25b0", FATAL, Logger::FIXME)
        << "cannot specify both '--database.check-version' and "
           "'--database.auto-upgrade'";
    FATAL_ERROR_EXIT();
  }
}

void DatabaseFeature::initCalculationVocbase(ArangodServer& server) {
  auto& df = server.getFeature<DatabaseFeature>();
  calculationVocbase = std::make_unique<TRI_vocbase_t>(
      createExpressionVocbaseInfo(server), df.versionTracker(),
      df.extendedNames(), /*isInternal*/ true);
}

void DatabaseFeature::start() {
#ifdef USE_V8
  auto& dealer = server().getFeature<V8DealerFeature>();
  if (dealer.isEnabled()) {
    dealer.verifyAppPaths();
  }
#endif

  // scan all databases
  VPackBuilder builder;
  _engine->getDatabases(builder);

  TRI_ASSERT(builder.slice().isArray());

  auto res = iterateDatabases(builder.slice());

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_TOPIC("0c49d", FATAL, Logger::FIXME)
        << "could not iterate over all databases: " << TRI_errno_string(res);
    FATAL_ERROR_EXIT();
  }

  if (!lookupDatabase(StaticStrings::SystemDatabase)) {
    LOG_TOPIC("97e7c", FATAL, Logger::FIXME)
        << "No _system database found in database directory. Cannot start!";
    FATAL_ERROR_EXIT();
  }

  // start database manager thread
  _databaseManager =
      std::make_unique<DatabaseManagerThread>(server(), *this, *_engine);

  if (!_databaseManager->start()) {
    LOG_TOPIC("7eb06", FATAL, Logger::FIXME)
        << "could not start database manager thread";
    FATAL_ERROR_EXIT();
  }

  // start IOHeartbeat thread:
  if ((ServerState::instance()->isDBServer() ||
       ServerState::instance()->isSingleServer() ||
       ServerState::instance()->isAgent()) &&
      _performIOHeartbeat) {
    _ioHeartbeatThread = std::make_unique<IOHeartbeatThread>(
        server(), server().getFeature<metrics::MetricsFeature>(),
        server().getFeature<DatabasePathFeature>());
    if (!_ioHeartbeatThread->start()) {
      LOG_TOPIC("7eb07", FATAL, Logger::FIXME)
          << "could not start IO check thread";
      FATAL_ERROR_EXIT();
    }
  }

  _started.store(true);
}

// signal to all databases that active cursors can be wiped
// this speeds up the actual shutdown because no waiting is necessary
// until the cursors happen to free their underlying transactions
void DatabaseFeature::beginShutdown() {
  if (_ioHeartbeatThread) {
    _ioHeartbeatThread->beginShutdown();  // will set thread state to STOPPING
    _ioHeartbeatThread->wakeup();         // will shorten the wait
  }

  auto databases = _databases.load();

  for (auto& p : *databases) {
    TRI_vocbase_t* vocbase = p.second;
    // iterate over all databases
    TRI_ASSERT(vocbase != nullptr);

    // throw away all open cursors in order to speed up shutdown
    vocbase->cursorRepository()->garbageCollect(true);
  }
}

void DatabaseFeature::stop() {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  // i am here for debugging only.
  static TRI_vocbase_t* currentVocbase = nullptr;
#endif

  stopAppliers();

  // turn off query cache and flush it
  aql::QueryCacheProperties p{
      .mode = aql::QueryCacheMode::CACHE_ALWAYS_OFF,
      .maxResultsCount = 0,
      .maxResultsSize = 0,
      .maxEntrySize = 0,
      .includeSystem = false,
      .showBindVars = false,
  };
  aql::QueryCache::instance()->properties(p);
  aql::QueryCache::instance()->invalidate();

  _engine->cleanupReplicationContexts();

  if (ServerState::instance()->isCoordinator()) {
    return;
  }

  std::unique_lock lock{_databasesMutex};
  auto databases = _databases.load();

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto queryRegistry = QueryRegistryFeature::registry();
  if (queryRegistry != nullptr) {
    TRI_ASSERT(queryRegistry->numberRegisteredQueries() == 0);
  }
#endif

  auto stopVocbase = [](TRI_vocbase_t* vocbase) {
    // iterate over all databases
    TRI_ASSERT(vocbase != nullptr);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // i am here for debugging only.
    currentVocbase = vocbase;
    static size_t currentCursorCount =
        currentVocbase->cursorRepository()->count();
    static size_t currentQueriesCount = currentVocbase->queryList()->count();

    LOG_TOPIC("840a4", DEBUG, Logger::FIXME)
        << "shutting down database " << currentVocbase->name() << ": "
        << static_cast<void*>(currentVocbase)
        << ", cursors: " << currentCursorCount
        << ", queries: " << currentQueriesCount;
#endif

    // Replicated logs are being processed in the vocbase->stop() method.
    // Note that it is necessary for replicated logs to be cleaned up only after
    // the maintenance thread has been stopped. Otherwise, the maintenance
    // thread may try to access the logs after they have been deleted. The
    // maintenance thread is stopped during ClusterFeature::stop().
    // The DatabaseFeature is stopped only after the ClusterFeature. Make sure
    // that we keep things in that order.
    vocbase->stop();

    vocbase->processCollectionsOnShutdown([](LogicalCollection* collection) {
      // no one else must modify the collection's status while we are in
      // here
      collection->executeWhileStatusWriteLocked(
          [collection]() { collection->close(); });
    });

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    // i am here for debugging only.
    LOG_TOPIC("4b2b7", DEBUG, Logger::FIXME)
        << "shutting down database " << currentVocbase->name() << ": "
        << static_cast<void*>(currentVocbase) << " successful";
#endif
  };

  for (auto& [name, vocbase] : *databases) {
    stopVocbase(vocbase);
  }

  for (auto& vocbase : _droppedDatabases) {
    stopVocbase(vocbase);
  }

  lock.unlock();

  // flush again so we are sure no query is left in the cache here
  aql::QueryCache::instance()->invalidate();
}

void DatabaseFeature::unprepare() {
  // delete the IO checker thread
  if (_ioHeartbeatThread != nullptr) {
    _ioHeartbeatThread->beginShutdown();

    while (_ioHeartbeatThread->isRunning()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // delete the database manager thread
  if (_databaseManager != nullptr) {
    _databaseManager->beginShutdown();

    while (_databaseManager->isRunning()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  try {
    closeDroppedDatabases();
  } catch (...) {
    // we're in the shutdown... simply ignore any errors produced here
  }

  _ioHeartbeatThread.reset();
  _databaseManager.reset();

  _pendingRecoveryCallbacks.clear();

  try {
    // closeOpenDatabases() can throw, but we're in a dtor
    closeOpenDatabases();
  } catch (...) {
  }
  calculationVocbase.reset();
}

void DatabaseFeature::prepare() {
  _engine = &server().getFeature<EngineSelectorFeature>().engine();
  if (server().hasFeature<ReplicationFeature>()) {
    _replicationFeature = &server().getFeature<ReplicationFeature>();
  }

  // need this to make calculation analyzer available in database links
  initCalculationVocbase(server());
}

void DatabaseFeature::recoveryDone() {
  TRI_ASSERT(!_engine->inRecovery());

  // '_pendingRecoveryCallbacks' will not change because
  // !StorageEngine.inRecovery()
  // It's single active thread before recovery done,
  // so we could use general purpose thread pool for this
  std::vector<futures::Future<Result>> futures;
  futures.reserve(_pendingRecoveryCallbacks.size());
  for (auto& entry : _pendingRecoveryCallbacks) {
    futures.emplace_back(SchedulerFeature::SCHEDULER->queueWithFuture(
        RequestLane::CLIENT_SLOW, std::move(entry)));
  }
  _pendingRecoveryCallbacks.clear();
  // TODO(MBkkt) use single wait with early termination
  // when it would be available
  for (auto& future : futures) {
    auto result = std::move(future).waitAndGet();
    if (!result.ok()) {
      LOG_TOPIC("772a7", ERR, Logger::FIXME)
          << "recovery failure due to error from callback, error '"
          << TRI_errno_string(result.errorNumber())
          << "' message: " << result.errorMessage();

      THROW_ARANGO_EXCEPTION(result);
    }
  }

  if (ServerState::instance()->isCoordinator()) {
    return;
  }

  auto databases = _databases.load();

  for (auto& p : *databases) {
    TRI_vocbase_t* vocbase = p.second;
    // iterate over all databases
    TRI_ASSERT(vocbase != nullptr);

    if (vocbase->replicationApplier() && _replicationFeature) {
      _replicationFeature->startApplier(vocbase);
    }
  }
}

Result DatabaseFeature::registerPostRecoveryCallback(
    std::function<Result()>&& callback) {
  if (!_engine->inRecovery()) {
    return callback();  // if no engine then can't be in recovery
  }

  // do not need a lock since single-thread access during recovery
  _pendingRecoveryCallbacks.emplace_back(std::move(callback));

  return Result();
}

bool DatabaseFeature::started() const noexcept {
  return _started.load(std::memory_order_relaxed);
}

void DatabaseFeature::enumerate(
    std::function<void(TRI_vocbase_t*)> const& callback) {
  auto databases = _databases.load();

  for (auto& p : *databases) {
    callback(p.second);
  }
}

Result DatabaseFeature::createDatabase(CreateDatabaseInfo&& info,
                                       TRI_vocbase_t*& result) {
  std::string name = info.getName();
  auto dbId = info.getId();
  velocypack::Builder markerBuilder;
  {
    velocypack::ObjectBuilder guard(&markerBuilder);
    info.toVelocyPack(markerBuilder);  // can we improve this
  }
  result = nullptr;

  std::unique_ptr<TRI_vocbase_t> vocbase;

  // create database in storage engine

  {
    // the create lock makes sure no one else is creating a database
    std::lock_guard lockCreate{_databaseCreateLock};
    {
      auto databases = _databases.load();

      auto it = databases->find(name);
      if (it != databases->end()) {
        // name already in use
        return Result(TRI_ERROR_ARANGO_DUPLICATE_NAME,
                      std::string("duplicate database name '") + name + "'");
      }

      if (ServerState::instance()->isSingleServerOrCoordinator() &&
          databases->size() >= maxDatabases()) {
        // intentionally do not validate number of databases on DB servers,
        // because they only carry out operations that are initiated by
        // coordinators
        return {TRI_ERROR_RESOURCE_LIMIT,
                absl::StrCat(
                    "unable to create additional database because it would "
                    "exceed the configured maximum number of databases (",
                    maxDatabases(), ")")};
      }
    }

    // createDatabase must return a valid database or throw
    vocbase = _engine->createDatabase(std::move(info));
    TRI_ASSERT(vocbase != nullptr);

    if (!ServerState::instance()->isCoordinator()) {
      try {
        vocbase->addReplicationApplier();
      } catch (basics::Exception const& ex) {
        std::string msg = "initializing replication applier for database '" +
                          vocbase->name() + "' failed: " + ex.what();
        LOG_TOPIC("e7444", ERR, Logger::FIXME) << msg;
        return Result(ex.code(), std::move(msg));
      } catch (std::exception const& ex) {
        std::string msg = "initializing replication applier for database '" +
                          vocbase->name() + "' failed: " + ex.what();
        LOG_TOPIC("56c41", ERR, Logger::FIXME) << msg;
        return Result(TRI_ERROR_INTERNAL, std::move(msg));
      }

#ifdef USE_V8
      auto& dealer = server().getFeature<V8DealerFeature>();
      if (dealer.isEnabled()) {
        auto r = dealer.createDatabase(name, std::to_string(dbId), true);
        if (r != TRI_ERROR_NO_ERROR) {
          THROW_ARANGO_EXCEPTION(r);
        }
      }
#endif
    }

    if (!_engine->inRecovery()) {
      if (!ServerState::instance()->isCoordinator() && _replicationFeature) {
        _replicationFeature->startApplier(vocbase.get());
      }

      // increase reference counter
      bool result = vocbase->use();
      TRI_ASSERT(result);
    }

    std::lock_guard lock{_databasesMutex};
    auto prev = _databases.load();

    auto next = _databases.make(prev);
    next->insert(std::make_pair(name, vocbase.get()));

    _databases.store(std::move(next));
    waitUnique(prev);
  }

  // write marker into log
  Result res;

  if (!_engine->inRecovery()) {
    res = _engine->writeCreateDatabaseMarker(dbId, markerBuilder.slice());
  }

  result = vocbase.release();

  versionTracker().track("create database");

  return res;
}

ErrorCode DatabaseFeature::dropDatabase(std::string_view name) {
  if (name == StaticStrings::SystemDatabase) {
    // prevent deletion of system database
    return TRI_ERROR_FORBIDDEN;
  }

  auto res = TRI_ERROR_NO_ERROR;
  {
    std::lock_guard lock{_databasesMutex};

    auto prev = _databases.load();

    auto it = prev->find(name);

    if (it == prev->end()) {
      return TRI_ERROR_ARANGO_DATABASE_NOT_FOUND;
    }

    TRI_vocbase_t* vocbase = it->second;

    // Shutdown and clear replicated logs here, while they still have access to
    // the vocbase. This also drops all shards and resources associated with
    // these replicated logs, so essentially there should be no collections left
    // in this vocbase after this method gets executed. Note that the remains of
    // the replicated logs will still be present on disk. They will be erased in
    // bulk while executing the `dropDatabase` method of the storage engine.
    vocbase->dropReplicatedLogs();

    auto next = _databases.make(prev);
    next->erase(name);

    _droppedDatabases.insert(vocbase);

    TRI_ASSERT(vocbase != nullptr);
    TRI_ASSERT(vocbase->id() != 0);

    _databases.store(std::move(next));
    waitUnique(prev);

    TRI_ASSERT(!vocbase->isSystem());
    // mark as deleted
    // it is fine to do this here already, because the vocbase cannot be deleted
    // while we are still holding the _databasesMutex. the DatabaseManagerThread
    // can only pick a database for deletion once it is in _droppedDatabases
    // *AND* it has acquired the _databasesMutex.
    bool result = vocbase->markAsDropped();
    TRI_ASSERT(result);

    // mark all collections in this database as deleted, too.
    try {
      auto collections = vocbase->collections(/*includeDeleted*/ false);
      for (auto& c : collections) {
        c->setDeleted();
        c->deferDropCollection(TRI_vocbase_t::dropCollectionCallback);
      }
    } catch (std::exception const& ex) {
      LOG_TOPIC("1f096", WARN, Logger::FIXME)
          << "error while dropping collections in database '" << vocbase->name()
          << "': " << ex.what();
    }

    // invalidate all entries for the database
    aql::QueryCache::instance()->invalidate(vocbase);

    if (server().hasFeature<iresearch::IResearchAnalyzerFeature>()) {
      server().getFeature<iresearch::IResearchAnalyzerFeature>().invalidate(
          *vocbase,
          transaction::OperationOriginInternal{"invalidating analyzers"});
    }
    auto queryRegistry = QueryRegistryFeature::registry();
    if (queryRegistry != nullptr) {
      queryRegistry->destroy(vocbase->name());
    }
    // TODO Temporary fix, this full method needs to be unified.
    try {
      vocbase->cursorRepository()->garbageCollect(true);
    } catch (...) {
    }

    res = _engine->prepareDropDatabase(*vocbase).errorNumber();
  }
  // must not use the database after here, as it may now be
  // deleted by the DatabaseManagerThread!

  versionTracker().track("drop database");

  return res;
}

ErrorCode DatabaseFeature::dropDatabase(TRI_voc_tick_t id) {
  std::string name;

  // find database by name
  {
    auto databases = _databases.load();

    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;

      if (vocbase->id() == id) {
        name = vocbase->name();
        break;
      }
    }
  }

  if (name.empty()) {
    return TRI_ERROR_ARANGO_DATABASE_NOT_FOUND;
  }
  // and call the regular drop function
  return dropDatabase(name);
}

std::vector<TRI_voc_tick_t> DatabaseFeature::getDatabaseIds(
    bool includeSystem) {
  std::vector<TRI_voc_tick_t> ids;

  {
    auto databases = _databases.load();

    ids.reserve(databases->size());

    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;
      TRI_ASSERT(vocbase != nullptr);
      if (vocbase->isDropped()) {
        continue;
      }
      if (includeSystem || vocbase->name() != StaticStrings::SystemDatabase) {
        ids.emplace_back(vocbase->id());
      }
    }
  }

  return ids;
}

std::vector<std::string> DatabaseFeature::getDatabaseNames() {
  std::vector<std::string> names;

  {
    auto databases = _databases.load();

    names.reserve(databases->size());

    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;
      TRI_ASSERT(vocbase != nullptr);
      if (vocbase->isDropped()) {
        continue;
      }
      names.emplace_back(vocbase->name());
    }
  }

  std::sort(names.begin(), names.end());

  return names;
}

std::vector<std::string> DatabaseFeature::getDatabaseNamesForUser(
    std::string const& username) {
  std::vector<std::string> names;

  AuthenticationFeature* af = AuthenticationFeature::instance();
  {
    auto databases = _databases.load();

    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;
      TRI_ASSERT(vocbase != nullptr);
      if (vocbase->isDropped()) {
        continue;
      }

      if (af->isActive() && af->userManager() != nullptr) {
        auto level =
            af->userManager()->databaseAuthLevel(username, vocbase->name());
        if (level == auth::Level::NONE) {  // hide dbs without access
          continue;
        }
      }

      names.emplace_back(vocbase->name());
    }
  }

  std::sort(names.begin(), names.end());

  return names;
}

void DatabaseFeature::inventory(
    velocypack::Builder& result, TRI_voc_tick_t maxTick,
    std::function<bool(LogicalCollection const*)> const& nameFilter) {
  result.openObject();
  {
    auto databases = _databases.load();

    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;
      TRI_ASSERT(vocbase != nullptr);
      if (vocbase->isDropped()) {
        continue;
      }

      result.add(vocbase->name(),
                 velocypack::Value(velocypack::ValueType::Object));
      result.add("id", velocypack::Value(std::to_string(vocbase->id())));
      result.add("name", velocypack::Value(vocbase->name()));
      vocbase->inventory(result, maxTick, nameFilter);
      result.close();
    }
  }
  result.close();
}

VocbasePtr DatabaseFeature::useDatabase(std::string_view name) const {
  auto databases = _databases.load();

  if (auto const it = databases->find(name); it != databases->end()) {
    TRI_vocbase_t* vocbase = it->second;
    if (vocbase->use()) {
      return VocbasePtr{vocbase};
    }
  }

  return nullptr;
}

VocbasePtr DatabaseFeature::useDatabase(TRI_voc_tick_t id) const {
  auto databases = _databases.load();

  for (auto& p : *databases) {
    auto* vocbase = p.second;

    if (vocbase->id() == id) {
      if (vocbase->use()) {
        return VocbasePtr{vocbase};
      }
      break;
    }
  }

  return nullptr;
}

bool DatabaseFeature::existsDatabase(std::string_view name) const {
  auto databases = _databases.load();
  return databases->contains(name);
}

TRI_vocbase_t* DatabaseFeature::lookupDatabase(std::string_view name) const {
  if (name.empty()) {
    return nullptr;
  }

  auto databases = _databases.load();

  // database names with a number in front are invalid names
  if (name[0] >= '0' && name[0] <= '9') {
    TRI_voc_tick_t id = NumberUtils::atoi_zero<TRI_voc_tick_t>(
        name.data(), name.data() + name.size());
    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;
      if (vocbase->id() == id) {
        return vocbase;
      }
    }
  } else {
    for (auto& p : *databases) {
      TRI_vocbase_t* vocbase = p.second;
      if (name == vocbase->name()) {
        return vocbase;
      }
    }
  }

  return nullptr;
}

std::string DatabaseFeature::translateCollectionName(
    std::string_view dbName, std::string_view collectionName) {
  auto databases = _databases.load();
  auto itr = databases->find(dbName);

  if (itr == databases->end()) {
    return std::string();
  }

  auto* vocbase = itr->second;
  TRI_ASSERT(vocbase != nullptr);

  if (ServerState::instance()->isCoordinator()) {
    CollectionNameResolver resolver(*vocbase);

    return resolver.getCollectionNameCluster(
        DataSourceId{NumberUtils::atoi_zero<DataSourceId::BaseType>(
            collectionName.data(),
            collectionName.data() + collectionName.size())});
  } else {
    auto collection = vocbase->lookupCollection(collectionName);

    return collection ? collection->name() : std::string();
  }
}

void DatabaseFeature::enumerateDatabases(
    std::function<void(TRI_vocbase_t& vocbase)> const& func) {
  auto databases = _databases.load();

  for (auto& p : *databases) {
    TRI_vocbase_t* vocbase = p.second;
    // iterate over all databases
    TRI_ASSERT(vocbase != nullptr);
    func(*vocbase);
  }
}

TRI_vocbase_t& DatabaseFeature::getCalculationVocbase() {
  TRI_ASSERT(calculationVocbase);
  return *calculationVocbase;
}

void DatabaseFeature::stopAppliers() {
  // stop the replication appliers so all replication transactions can end
  if (_replicationFeature == nullptr) {
    return;
  }

  std::lock_guard lock{_databasesMutex};
  auto databases = _databases.load();

  for (auto& p : *databases) {
    TRI_vocbase_t* vocbase = p.second;
    TRI_ASSERT(vocbase != nullptr);
    if (!ServerState::instance()->isCoordinator()) {
      _replicationFeature->stopApplier(vocbase);
    }
  }
}

void DatabaseFeature::closeOpenDatabases() {
  std::unique_lock lock{_databasesMutex};

  // Build the new value:
  auto databases = _databases.load();

  // Replace the old by the new:
  _databases.store(_databases.create());
  waitUnique(databases);
  lock.unlock();

  // Now it is safe to destroy the old databases:
  for (auto& p : *databases) {
    TRI_vocbase_t* vocbase = p.second;
    TRI_ASSERT(vocbase != nullptr);
    vocbase->shutdown();

    delete vocbase;
  }
}

ErrorCode DatabaseFeature::iterateDatabases(velocypack::Slice databases) {
#ifdef USE_V8
  auto& dealer = server().getFeature<V8DealerFeature>();
#endif

  auto r = TRI_ERROR_NO_ERROR;

  // open databases in defined order
  std::lock_guard lock{_databasesMutex};

  auto prev = _databases.load();
  auto next = _databases.make(prev);

  std::vector<TRI_vocbase_t*> newDatabases;
  auto databaseGuard = scopeGuard([&newDatabases]() noexcept {
    for (auto p : newDatabases) {
      delete p;
    }
  });

  ServerState::RoleEnum role = ServerState::instance()->getRole();

  for (velocypack::Slice it : velocypack::ArrayIterator(databases)) {
    TRI_ASSERT(it.isObject());
    LOG_TOPIC("95f68", TRACE, Logger::FIXME)
        << "processing database: " << it.toJson();

    velocypack::Slice deleted = it.get("deleted");
    if (deleted.isTrue()) {
      // ignore deleted databases here
      continue;
    }

    auto name = it.get("name").stringView();
#ifdef USE_V8
    if (dealer.isEnabled()) {
      auto id = basics::VelocyPackHelper::getStringView(it.get("id"), {});
      r = dealer.createDatabase(name, id, false);
      if (r != TRI_ERROR_NO_ERROR) {
        break;
      }
    }
#endif

    // open the database and scan collections in it

    // try to open this database
    CreateDatabaseInfo info(server(), ExecContext::current());
    // set strict validation for database options to false.
    // we don't want the server start to fail here in case some
    // invalid settings are present
    info.strictValidation(false);

    // do not validate database names for existing databases.
    // the rationale is that if a database was already created with
    // an extended name, we should not declare it invalid and abort
    // the startup once the extended names option is turned off.
    info.validateNames(false);

    auto res = info.load(it, velocypack::Slice::emptyArraySlice());

    if (res.fail()) {
      std::string errorMsg;
      // note: TRI_ERROR_ARANGO_DATABASE_NAME_INVALID should not be
      // used anymore in 3.11 and higher.
      bool isNameError = res.is(TRI_ERROR_ARANGO_DATABASE_NAME_INVALID) ||
                         res.is(TRI_ERROR_ARANGO_ILLEGAL_NAME);
      if (isNameError) {
        // special case: if we find an invalid database name during startup,
        // we will give the user some hint how to fix it
        absl::StrAppend(&errorMsg, res.errorMessage(), ": '", name, "'");
        // check if the name would be allowed when using extended names
        if (DatabaseNameValidator::validateName(
                /*isSystem*/ false, /*extendedNames*/ true, name)
                .ok()) {
          errorMsg.append(
              ". This database name would be allowed when using the "
              "extended naming convention for databases, which is "
              "currently disabled. The extended naming convention can "
              "be enabled via the startup option "
              "`--database.extended-names true`");
        }
      } else {
        absl::StrAppend(&errorMsg, "when opening database '", name,
                        "': ", res.errorMessage());
      }

      if (!isNameError) {
        // rethrow all errors but "illegal name" errors.
        // the "illegal name" errors will be silenced during startup,
        // so that it will be possible to start a database server
        // with existing databases with extended database names even
        // if the extended names feature was turned off later.
        res.reset(res.errorNumber(), std::move(errorMsg));
        THROW_ARANGO_EXCEPTION(res);
      }
    }

    auto database = _engine->openDatabase(std::move(info), _upgrade);

    if (!ServerState::isCoordinator(role) && !ServerState::isAgent(role)) {
      try {
        database->addReplicationApplier();
      } catch (std::exception const& ex) {
        LOG_TOPIC("ff848", FATAL, Logger::FIXME)
            << "initializing replication applier for database '"
            << database->name() << "' failed: " << ex.what();
        FATAL_ERROR_EXIT();
      }
    }
    ADB_PROD_ASSERT(!next->contains(database->name()))
        << "duplicate database name " << database->name();
    next->emplace(database->name(), database.get());
    newDatabases.emplace_back(database.release());
  }

  databaseGuard.cancel();
  _databases.store(std::move(next));
  waitUnique(prev);

  return r;
}

void DatabaseFeature::closeDroppedDatabases() {
  std::unique_lock lock{_databasesMutex};
  // take a copy
  auto dropped = std::move(_droppedDatabases);
  _droppedDatabases.clear();
  lock.unlock();

  auto guard = scopeGuard([&dropped]() noexcept {
    for (auto p : dropped) {
      delete p;
    }
  });

  if (!ServerState::instance()->isCoordinator()) {
    return;
  }

  for (auto p : dropped) {
    p->shutdown();
  }
}

}  // namespace arangodb
