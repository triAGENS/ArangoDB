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
/// @author Dan Larkin-York
////////////////////////////////////////////////////////////////////////////////

#include "ManagerTasks.h"

#include "Basics/ScopeGuard.h"
#include "Basics/SpinLocker.h"
#include "Basics/debugging.h"
#include "Cache/Cache.h"
#include "Cache/Manager.h"
#include "Cache/Metadata.h"
#include "Logger/LogMacros.h"
#include "Random/RandomGenerator.h"

#include <chrono>

namespace arangodb::cache {

FreeMemoryTask::FreeMemoryTask(Manager::TaskEnvironment environment,
                               Manager& manager, std::shared_ptr<Cache> cache)
    : _environment(environment), _manager(manager), _cache(std::move(cache)) {}

FreeMemoryTask::~FreeMemoryTask() = default;

bool FreeMemoryTask::dispatch() {
  // prepareTask counts a counter up
  _manager.prepareTask(_environment);

  // make sure we count the counter down in case we
  // did not successfully dispatch the task
  auto unprepareGuard = scopeGuard([this]() noexcept {
    // we need to set internal=true here, because unprepareTask()
    // can acquire the manager's lock in write mode, which we currently
    // are already holding. internal=true prevents us from deadlocking
    // when calling unprepareTask() from here.
    _manager.unprepareTask(_environment, /*internal*/ true);
  });

  TRI_IF_FAILURE("CacheManagerTasks::dispatchFailures") {
    if (RandomGenerator::interval(uint32_t(100)) >= 70) {
      return false;
    }
  }

  if (_manager.post([self = shared_from_this()]() -> void { self->run(); })) {
    // intentionally don't unprepare task
    unprepareGuard.cancel();
    return true;
  }
  return false;
}

void FreeMemoryTask::run() {
  using basics::SpinLocker;

  auto unprepareGuard = scopeGuard([this]() noexcept {
    // here we need to set internal=false as we are not holding
    // the manager's lock right now.
    _manager.unprepareTask(_environment, /*internal*/ false);
  });

  TRI_ASSERT(_cache->isResizingFlagSet());

  auto toggleResizingGuard = scopeGuard([this]() noexcept {
    // make sure we are always clearing the resizing flag
    Metadata& metadata = _cache->metadata();
    SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());
    TRI_ASSERT(metadata.isResizing());
    metadata.toggleResizing();
    TRI_ASSERT(!metadata.isResizing());
  });

  // execute freeMemory() with timing
  auto now = std::chrono::steady_clock::now();
  bool ran = _cache->freeMemory();
  auto diff = std::chrono::steady_clock::now() - now;
  _manager.trackFreeMemoryTaskDuration(
      std::chrono::duration_cast<std::chrono::microseconds>(diff).count());

  // flag must still be set after freeMemory()
  TRI_ASSERT(_cache->isResizingFlagSet());

  if (ran) {
    std::uint64_t reclaimed = 0;
    SpinLocker guard(SpinLocker::Mode::Write, _manager._lock);
    Metadata& metadata = _cache->metadata();
    {
      SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());
      TRI_ASSERT(metadata.isResizing());
      reclaimed = metadata.hardUsageLimit - metadata.softUsageLimit;
      bool ok = metadata.adjustLimits(metadata.softUsageLimit,
                                      metadata.softUsageLimit);
      metadata.toggleResizing();
      TRI_ASSERT(!metadata.isResizing());

      if (ok) {
        TRI_ASSERT(_manager._globalAllocation >=
                   reclaimed + _manager._fixedAllocation);
        _manager._globalAllocation -= reclaimed;
        TRI_ASSERT(_manager._globalAllocation >= _manager._fixedAllocation);
      }
    }
    // do not toggle the resizing flag twice
    toggleResizingGuard.cancel();
  }

  LOG_TOPIC("dce52", TRACE, Logger::CACHE)
      << "freeMemory task took "
      << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count()
      << "ms";
}

MigrateTask::MigrateTask(Manager::TaskEnvironment environment, Manager& manager,
                         std::shared_ptr<Cache> cache,
                         std::shared_ptr<Table> table)
    : _environment(environment),
      _manager(manager),
      _cache(std::move(cache)),
      _table(std::move(table)) {}

MigrateTask::~MigrateTask() = default;

bool MigrateTask::dispatch() {
  // prepareTask counts a counter up
  _manager.prepareTask(_environment);

  // make sure we count the counter down in case we
  // did not successfully dispatch the task
  auto unprepareGuard = scopeGuard([this]() noexcept {
    // we need to set internal=true here, because unprepareTask()
    // can acquire the manager's lock in write mode, which we currently
    // are already holding. internal=true prevents us from deadlocking
    // when calling unprepareTask() from here.
    _manager.unprepareTask(_environment, /*internal*/ true);
  });

  TRI_IF_FAILURE("CacheManagerTasks::dispatchFailures") {
    if (RandomGenerator::interval(uint32_t(100)) >= 70) {
      return false;
    }
  }

  if (_manager.post([self = shared_from_this()]() -> void { self->run(); })) {
    // intentionally don't unprepare task
    unprepareGuard.cancel();
    return true;
  }
  return false;
}

void MigrateTask::run() {
  auto unprepareGuard = scopeGuard([this]() noexcept {
    // here we need to set internal=false as we are not holding
    // the manager's lock right now.
    _manager.unprepareTask(_environment, /*internal*/ false);
  });

  // we must be migrating when we get here
  TRI_ASSERT(_cache->isMigratingFlagSet());

  // do the actual migration
  auto now = std::chrono::steady_clock::now();
  bool ran = _cache->migrate(_table);
  auto diff = std::chrono::steady_clock::now() - now;
  _manager.trackMigrateTaskDuration(
      std::chrono::duration_cast<std::chrono::microseconds>(diff).count());

  LOG_TOPIC("f4c44", TRACE, Logger::CACHE)
      << "migrate task on table with " << _table->size() << " slots took "
      << std::chrono::duration_cast<std::chrono::milliseconds>(diff).count()
      << "ms";

  // migrate() must have unset the migrating flag, but we
  // cannot check it here because another MigrateTask may
  // have been scheduled in the meantime and have set the
  // flag again, which would be a valid situation.

  if (!ran) {
    _manager.reclaimTable(std::move(_table), false);
    TRI_ASSERT(_table == nullptr);
  }
}
}  // namespace arangodb::cache
