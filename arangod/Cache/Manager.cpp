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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <boost/fiber/detail/cpu_relax.hpp>

#include "Cache/Manager.h"

#include "Basics/SpinLocker.h"
#include "Basics/SpinUnlocker.h"
#include "Basics/voc-errors.h"
#include "Cache/BinaryKeyHasher.h"
#include "Cache/Cache.h"
#include "Cache/CachedValue.h"
#include "Cache/Common.h"
#include "Cache/FrequencyBuffer.h"
#include "Cache/ManagerTasks.h"
#include "Cache/Metadata.h"
#include "Cache/PlainCache.h"
#include "Cache/Table.h"
#include "Cache/Transaction.h"
#include "Cache/TransactionalCache.h"
#include "Cache/VPackKeyHasher.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "RestServer/SharedPRNGFeature.h"

namespace arangodb::cache {

using SpinLocker = ::arangodb::basics::SpinLocker;
using SpinUnlocker = ::arangodb::basics::SpinUnlocker;

// note: the usage of BinaryKeyHasher here is arbitrary. actually all
// hashers should be stateless and thus there should be no size difference
// between them
const std::uint64_t Manager::minCacheAllocation =
    Cache::kMinSize + Table::allocationSize(Table::kMinLogSize) +
    std::max(PlainCache<BinaryKeyHasher>::allocationSize(true),
             TransactionalCache<BinaryKeyHasher>::allocationSize(true)) +
    Manager::cacheRecordOverhead;

Manager::Manager(SharedPRNGFeature& sharedPRNG, PostFn schedulerPost,
                 std::uint64_t globalLimit, bool enableWindowedStats)
    : _sharedPRNG(sharedPRNG),
      _lock(),
      _shutdown(false),
      _shuttingDown(false),
      _resizing(false),
      _rebalancing(false),
      _accessStats(sharedPRNG,
                   (globalLimit >= (1024 * 1024 * 1024))
                       ? ((1024 * 1024) / sizeof(std::uint64_t))
                       : (globalLimit / (1024 * sizeof(std::uint64_t)))),
      _enableWindowedStats(enableWindowedStats),
      _findHits(),
      _findMisses(),
      _caches(),
      _nextCacheId(1),
      _globalSoftLimit(globalLimit),
      _globalHardLimit(globalLimit),
      _globalHighwaterMark(
          static_cast<std::uint64_t>(Manager::highwaterMultiplier *
                                     static_cast<double>(_globalSoftLimit))),
      _fixedAllocation(sizeof(Manager) + Manager::tableListsOverhead +
                       _accessStats.memoryUsage()),
      _spareTableAllocation(0),
      _globalAllocation(_fixedAllocation),
      _activeTables(0),
      _spareTables(0),
      _transactions(),
      _schedulerPost(std::move(schedulerPost)),
      _outstandingTasks(0),
      _rebalancingTasks(0),
      _resizingTasks(0),
      _rebalanceCompleted(std::chrono::steady_clock::now() -
                          Manager::rebalancingGracePeriod) {
  TRI_ASSERT(_globalAllocation < _globalSoftLimit);
  TRI_ASSERT(_globalAllocation < _globalHardLimit);
  if (enableWindowedStats) {
    try {
      _findStats = std::make_unique<Manager::FindStatBuffer>(sharedPRNG, 16384);
      _fixedAllocation += _findStats->memoryUsage();
      _globalAllocation = _fixedAllocation;
    } catch (std::bad_alloc const&) {
      _findStats.reset();
      _enableWindowedStats = false;
    }
  }
}

Manager::~Manager() {
  try {
    shutdown();
  } catch (...) {
    // no exceptions allowed here
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  SpinLocker guard(SpinLocker::Mode::Read, _lock);
  TRI_ASSERT(_globalAllocation == _fixedAllocation)
      << "globalAllocation: " << _globalAllocation
      << ", fixedAllocation: " << _fixedAllocation
      << ", outstandingTasks: " << _outstandingTasks;
#endif
}

template<typename Hasher>
std::shared_ptr<Cache> Manager::createCache(CacheType type,
                                            bool enableWindowedStats,
                                            std::uint64_t maxSize) {
  std::shared_ptr<Cache> result;
  {
    Metadata metadata;
    std::shared_ptr<Table> table;

    SpinLocker guard(SpinLocker::Mode::Write, _lock);
    bool allowed = isOperational();

    if (allowed) {
      std::uint64_t fixedSize = [&type, &enableWindowedStats]() {
        switch (type) {
          case CacheType::Plain:
            return PlainCache<Hasher>::allocationSize(enableWindowedStats);
          case CacheType::Transactional:
            return TransactionalCache<Hasher>::allocationSize(
                enableWindowedStats);
          default:
            return std::uint64_t(0);
        }
      }();
      std::tie(allowed, metadata, table) = registerCache(fixedSize, maxSize);
    }

    // note: allowed can be overwritten by registerCache()
    if (allowed) {
      std::uint64_t id = _nextCacheId++;

      switch (type) {
        case CacheType::Plain:
          result =
              PlainCache<Hasher>::create(this, id, std::move(metadata),
                                         std::move(table), enableWindowedStats);
          break;
        case CacheType::Transactional:
          result = TransactionalCache<Hasher>::create(
              this, id, std::move(metadata), std::move(table),
              enableWindowedStats);
          break;
        default:
          break;
      }

      if (result != nullptr) {
        _caches.try_emplace(id, result);
      }
    }
  }

  return result;
}

void Manager::destroyCache(std::shared_ptr<Cache> const& cache) {
  Cache::destroy(cache);
}

void Manager::beginShutdown() {
  SpinLocker guard(SpinLocker::Mode::Write, _lock);

  if (!_shutdown) {
    _shuttingDown = true;
  }
}

void Manager::shutdown() {
  SpinLocker guard(SpinLocker::Mode::Write, _lock);

  if (!_shutdown) {
    if (!_shuttingDown) {
      _shuttingDown = true;
    }

    while (globalProcessRunning()) {
      // wait for rebalancer and migration tasks to complete
      cpu_relax()
    }

    while (!_caches.empty()) {
      std::shared_ptr<Cache> cache = _caches.begin()->second;
      SpinUnlocker unguard(SpinUnlocker::Mode::Write, _lock);
      cache->shutdown();
    }
    freeUnusedTables();
    _shutdown = true;
  }
}

// change global cache limit
bool Manager::resize(std::uint64_t newGlobalLimit) {
  SpinLocker guard(SpinLocker::Mode::Write, _lock);

  if ((newGlobalLimit < Manager::kMinSize) ||
      (static_cast<std::uint64_t>(0.5 * (1.0 - Manager::highwaterMultiplier) *
                                  static_cast<double>(newGlobalLimit)) <
       _fixedAllocation) ||
      (static_cast<std::uint64_t>(Manager::highwaterMultiplier *
                                  static_cast<double>(newGlobalLimit)) <
       (_caches.size() * Manager::minCacheAllocation))) {
    return false;
  }

  bool success = true;
  if (!isOperational() || globalProcessRunning()) {
    // shut(ting) down or still have another global process running already
    success = false;
  } else {
    bool done = adjustGlobalLimitsIfAllowed(newGlobalLimit);
    if (!done) {
      // otherwise we need to actually resize
      _resizing = true;
      _globalSoftLimit = newGlobalLimit;
      _globalHighwaterMark = static_cast<std::uint64_t>(
          Manager::highwaterMultiplier * static_cast<double>(_globalSoftLimit));
      freeUnusedTables();
      done = adjustGlobalLimitsIfAllowed(newGlobalLimit);
      if (!done) {
        rebalance(true);
        shrinkOvergrownCaches(TaskEnvironment::resizing);
      }
    }
  }

  return success;
}

std::uint64_t Manager::globalLimit() const noexcept {
  SpinLocker guard(SpinLocker::Mode::Read, _lock);
  return _resizing ? _globalSoftLimit : _globalHardLimit;
}

std::uint64_t Manager::globalAllocation() const noexcept {
  SpinLocker guard(SpinLocker::Mode::Read, _lock);
  TRI_ASSERT(_globalAllocation >= _fixedAllocation);
  return _globalAllocation;
}

std::uint64_t Manager::spareAllocation() const noexcept {
  SpinLocker guard(SpinLocker::Mode::Read, _lock);
  return _spareTableAllocation;
}

Manager::MemoryStats Manager::memoryStats() const noexcept {
  Manager::MemoryStats result;

  SpinLocker guard(SpinLocker::Mode::Read, _lock);
  result.globalLimit = _resizing ? _globalSoftLimit : _globalHardLimit;
  result.globalAllocation = _globalAllocation;
  result.spareAllocation = _spareTableAllocation;
  result.activeTables = _activeTables;
  result.spareTables = _spareTables;

  return result;
}

std::pair<double, double> Manager::globalHitRates() {
  double lifetimeRate = std::nan("");
  double windowedRate = std::nan("");

  std::uint64_t currentHits = _findHits.value(std::memory_order_relaxed);
  std::uint64_t currentMisses = _findMisses.value(std::memory_order_relaxed);
  if (currentHits + currentMisses > 0) {
    lifetimeRate = 100.0 * (static_cast<double>(currentHits) /
                            static_cast<double>(currentHits + currentMisses));
  }

  if (_enableWindowedStats && _findStats.get() != nullptr) {
    auto stats = _findStats->getFrequencies();
    if (stats.size() == 1) {
      if (stats[0].first == static_cast<std::uint8_t>(Stat::findHit)) {
        windowedRate = 100.0;
      } else {
        windowedRate = 0.0;
      }
    } else if (stats.size() == 2) {
      if (stats[0].first == static_cast<std::uint8_t>(Stat::findHit)) {
        currentHits = stats[0].second;
        currentMisses = stats[1].second;
      } else {
        currentHits = stats[1].second;
        currentMisses = stats[0].second;
      }
      if (currentHits + currentMisses > 0) {
        windowedRate =
            100.0 * (static_cast<double>(currentHits) /
                     static_cast<double>(currentHits + currentMisses));
      }
    }
  }

  return std::make_pair(lifetimeRate, windowedRate);
}

Transaction* Manager::beginTransaction(bool readOnly) {
  return _transactions.begin(readOnly);
}

void Manager::endTransaction(Transaction* tx) noexcept {
  _transactions.end(tx);
}

bool Manager::post(std::function<void()> fn) {
  // lock already acquired by caller
  TRI_ASSERT(_lock.isLockedWrite());
  return _schedulerPost(std::move(fn));
}

std::tuple<bool, Metadata, std::shared_ptr<Table>> Manager::registerCache(
    std::uint64_t fixedSize, std::uint64_t maxSize) {
  TRI_ASSERT(_lock.isLockedWrite());
  Metadata metadata;
  std::shared_ptr<Table> table;
  bool ok = true;

  if ((_globalHighwaterMark / (_caches.size() + 1)) <
      Manager::minCacheAllocation) {
    ok = false;
  }

  if (ok) {
    table = leaseTable(Table::kMinLogSize);
    ok = (table != nullptr);
  }

  if (ok) {
    std::uint64_t memoryUsage = table->memoryUsage();
    metadata = Metadata(Cache::kMinSize, fixedSize, memoryUsage, maxSize);
    TRI_ASSERT(metadata.allocatedSize >= memoryUsage);
    ok = increaseAllowed(metadata.allocatedSize - memoryUsage, true);
    if (ok) {
      TRI_ASSERT(_globalAllocation + (metadata.allocatedSize - memoryUsage) >=
                 _fixedAllocation);
      _globalAllocation += (metadata.allocatedSize - memoryUsage);
      TRI_ASSERT(_globalAllocation >= _fixedAllocation);
    }
  }

  if (!ok && (table != nullptr)) {
    reclaimTable(std::move(table), true);
    table.reset();
  }

  return std::make_tuple(ok, std::move(metadata), std::move(table));
}

void Manager::unregisterCache(std::uint64_t id) {
  SpinLocker guard(SpinLocker::Mode::Write, _lock);
  _accessStats.purgeRecord(id);
  auto it = _caches.find(id);
  if (it == _caches.end()) {
    return;
  }
  std::shared_ptr<Cache>& cache = it->second;
  Metadata& metadata = cache->metadata();
  {
    SpinLocker metaGuard(SpinLocker::Mode::Read, metadata.lock());
    TRI_ASSERT(_globalAllocation >= metadata.allocatedSize + _fixedAllocation);
    _globalAllocation -= metadata.allocatedSize;
    TRI_ASSERT(_globalAllocation >= _fixedAllocation);
  }
  _caches.erase(id);
}

std::pair<bool, Manager::time_point> Manager::requestGrow(Cache* cache) {
  Manager::time_point nextRequest = futureTime(100);
  bool allowed = false;

  SpinLocker guard(SpinLocker::Mode::Write, _lock,
                   static_cast<std::size_t>(Manager::triesSlow));
  if (guard.isLocked()) {
    if (isOperational() && !globalProcessRunning()) {
      Metadata& metadata = cache->metadata();
      SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());

      allowed = !metadata.isResizing() && !metadata.isMigrating();
      if (allowed) {
        if (metadata.allocatedSize >= metadata.deservedSize &&
            pastRebalancingGracePeriod()) {
          std::uint64_t increase =
              std::min(metadata.hardUsageLimit / 2,
                       metadata.maxSize - metadata.allocatedSize);
          if (increase > 0 && increaseAllowed(increase)) {
            std::uint64_t newLimit = metadata.allocatedSize + increase;
            metadata.adjustDeserved(newLimit);
          } else {
            allowed = false;
          }
        }

        if (allowed) {
          nextRequest = std::chrono::steady_clock::now();
          resizeCache(TaskEnvironment::none, std::move(metaGuard), cache,
                      metadata.newLimit());  // unlocks metadata
        }
      }
    }
  }

  return std::make_pair(allowed, nextRequest);
}

std::pair<bool, Manager::time_point> Manager::requestMigrate(
    Cache* cache, std::uint32_t requestedLogSize) {
  Manager::time_point nextRequest = futureTime(100);
  bool allowed = false;

  SpinLocker guard(SpinLocker::Mode::Write, _lock,
                   static_cast<std::size_t>(Manager::triesSlow));
  if (guard.isLocked()) {
    if (isOperational() && !globalProcessRunning()) {
      Metadata& metadata = cache->metadata();
      SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());

      allowed = !metadata.isMigrating();
      if (allowed) {
        if (metadata.tableSize < Table::allocationSize(requestedLogSize)) {
          std::uint64_t increase =
              Table::allocationSize(requestedLogSize) - metadata.tableSize;
          if ((metadata.allocatedSize + increase >= metadata.deservedSize) &&
              pastRebalancingGracePeriod()) {
            if (increaseAllowed(increase)) {
              std::uint64_t newLimit = metadata.allocatedSize + increase;
              std::uint64_t granted = metadata.adjustDeserved(newLimit);
              if (granted < newLimit) {
                allowed = false;
              }
            } else {
              allowed = false;
            }
          }
        }
      }

      if (allowed) {
        // first find out if cache is allowed to migrate
        allowed =
            metadata.migrationAllowed(Table::allocationSize(requestedLogSize));
      }
      if (allowed) {
        // now find out if we can lease the table
        std::shared_ptr<Table> table = leaseTable(requestedLogSize);
        allowed = (table != nullptr);
        if (allowed) {
          nextRequest = std::chrono::steady_clock::now();
          migrateCache(TaskEnvironment::none, std::move(metaGuard), cache,
                       std::move(table));  // unlocks metadata
        }
      }
    }
  }

  return std::make_pair(allowed, nextRequest);
}

void Manager::reportAccess(std::uint64_t id) noexcept {
  if ((_sharedPRNG.rand() & static_cast<unsigned long>(7)) == 0) {
    _accessStats.insertRecord(id);
  }
}

void Manager::reportHitStat(Stat stat) noexcept {
  switch (stat) {
    case Stat::findHit: {
      _findHits.add(1, std::memory_order_relaxed);
      if (_enableWindowedStats && _findStats != nullptr) {
        _findStats->insertRecord(static_cast<std::uint8_t>(Stat::findHit));
      }
      break;
    }
    case Stat::findMiss: {
      _findMisses.add(1, std::memory_order_relaxed);
      if (_enableWindowedStats && _findStats != nullptr) {
        _findStats->insertRecord(static_cast<std::uint8_t>(Stat::findMiss));
      }
      break;
    }
    default: {
      break;
    }
  }
}

bool Manager::isOperational() const noexcept {
  TRI_ASSERT(_lock.isLocked());
  return (!_shutdown && !_shuttingDown);
}

bool Manager::globalProcessRunning() const noexcept {
  TRI_ASSERT(_lock.isLocked());
  return (_rebalancing || _resizing);
}

void Manager::prepareTask(Manager::TaskEnvironment environment) {
  // lock already acquired by caller
  TRI_ASSERT(_lock.isLockedWrite());

  ++_outstandingTasks;
  switch (environment) {
    case TaskEnvironment::rebalancing: {
      ++_rebalancingTasks;
      break;
    }
    case TaskEnvironment::resizing: {
      ++_resizingTasks;
      break;
    }
    case TaskEnvironment::none:
    default: {
      break;
    }
  }
}

void Manager::unprepareTask(Manager::TaskEnvironment environment) noexcept {
  switch (environment) {
    case TaskEnvironment::rebalancing: {
      TRI_ASSERT(_rebalancingTasks > 0);
      if (--_rebalancingTasks == 0) {
        SpinLocker guard(SpinLocker::Mode::Write, _lock);
        _rebalancing = false;
        _rebalanceCompleted = std::chrono::steady_clock::now();
      }
      break;
    }
    case TaskEnvironment::resizing: {
      TRI_ASSERT(_resizingTasks > 0);
      if (--_resizingTasks == 0) {
        SpinLocker guard(SpinLocker::Mode::Write, _lock);
        _resizing = false;
      }
      break;
    }
    case TaskEnvironment::none:
    default: {
      break;
    }
  }

  --_outstandingTasks;
}

/// TODO Improve rebalancing algorithm
/// Currently our allocations are heavily based on usage frequency, which can
/// lead to widly oscillating sizes and significant thrashing via background
/// free memory tasks. Also, we currently do not attempt to shrink tables, just
/// free memory from them. It may behoove us to revisit the algorithm. A
/// discussion some years ago ended with the idea to institute a system inspired
/// by redistributive taxation. At the beginning of each rebalancing period,
/// ask each cache what limit it would like: if it needs more space, could give
/// up some space, or if it is happy. If at least one cache says it needs more
/// space, then collect a tax from each cache, say 5% of its current allocation.
/// Then, given the pool of memory, and the expressed needs of each cache,
/// attempt to allocate memory evenly, up to the additional amount requested.
ErrorCode Manager::rebalance(bool onlyCalculate) {
  SpinLocker guard(SpinLocker::Mode::Write, _lock, !onlyCalculate);

  if (!onlyCalculate) {
    if (_caches.empty()) {
      return TRI_ERROR_NO_ERROR;
    }
    if (!isOperational()) {
      return TRI_ERROR_SHUTTING_DOWN;
    }
    if (globalProcessRunning()) {
      return TRI_ERROR_ARANGO_BUSY;
    }

    // start rebalancing
    _rebalancing = true;
  }

  // adjust deservedSize for each cache
  std::shared_ptr<PriorityList> cacheList = priorityList();
  for (auto pair : (*cacheList)) {
    std::shared_ptr<Cache>& cache = pair.first;
    double weight = pair.second;
    auto newDeserved = static_cast<std::uint64_t>(
        std::ceil(weight * static_cast<double>(_globalHighwaterMark)));
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    if (newDeserved < Manager::minCacheAllocation) {
      LOG_TOPIC("eabec", DEBUG, Logger::CACHE)
          << "Deserved limit of " << newDeserved << " from weight " << weight
          << " and highwater " << _globalHighwaterMark
          << ". Should be at least " << Manager::minCacheAllocation;
      TRI_ASSERT(newDeserved >= Manager::minCacheAllocation);
    }
#endif
    Metadata& metadata = cache->metadata();
    SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    std::uint64_t fixed =
        metadata.fixedSize + metadata.tableSize + Manager::cacheRecordOverhead;
    if (newDeserved < fixed) {
      LOG_TOPIC("e63e4", DEBUG, Logger::CACHE)
          << "Setting deserved cache size " << newDeserved
          << " below usage: " << fixed << " ; Using weight  " << weight;
    }
#endif
    metadata.adjustDeserved(newDeserved);
  }

  if (!onlyCalculate) {
    if (_globalAllocation >= _globalHighwaterMark * 0.7) {
      shrinkOvergrownCaches(TaskEnvironment::rebalancing);
    }

    if (_rebalancingTasks.load() == 0) {
      _rebalanceCompleted = std::chrono::steady_clock::now();
      _rebalancing = false;
    }
  }

  return TRI_ERROR_NO_ERROR;
}

void Manager::shrinkOvergrownCaches(Manager::TaskEnvironment environment) {
  TRI_ASSERT(_lock.isLockedWrite());
  for (auto& [_, cache] : _caches) {
    // skip this cache if it is already resizing or shutdown!
    if (!cache->canResize()) {
      continue;
    }

    Metadata& metadata = cache->metadata();
    SpinLocker metaGuard(SpinLocker::Mode::Write, metadata.lock());

    if (metadata.allocatedSize > metadata.deservedSize) {
      resizeCache(environment, std::move(metaGuard), cache.get(),
                  metadata.newLimit());  // unlocks metadata
    }
  }
}

void Manager::freeUnusedTables() {
  TRI_ASSERT(_lock.isLockedWrite());

  constexpr std::size_t tableEntries =
      std::tuple_size<decltype(_tables)>::value;

  for (std::size_t i = 0; i < tableEntries; i++) {
    while (!_tables[i].empty()) {
      auto& table = _tables[i].top();
      std::uint64_t memoryUsage = table->memoryUsage();
      TRI_ASSERT(_globalAllocation >= memoryUsage + _fixedAllocation);
      _globalAllocation -= memoryUsage;
      TRI_ASSERT(_globalAllocation >= _fixedAllocation);

      TRI_ASSERT(_spareTableAllocation >= memoryUsage);
      _spareTableAllocation -= memoryUsage;

      TRI_ASSERT(_spareTables > 0);

      --_spareTables;
      _tables[i].pop();
    }
  }
}

bool Manager::adjustGlobalLimitsIfAllowed(std::uint64_t newGlobalLimit) {
  TRI_ASSERT(_lock.isLockedWrite());
  if (newGlobalLimit < _globalAllocation) {
    return false;
  }

  _globalHighwaterMark = static_cast<std::uint64_t>(
      Manager::highwaterMultiplier * static_cast<double>(newGlobalLimit));
  _globalSoftLimit = newGlobalLimit;
  _globalHardLimit = newGlobalLimit;

  return true;
}

void Manager::resizeCache(Manager::TaskEnvironment environment,
                          SpinLocker&& metaGuard, Cache* cache,
                          std::uint64_t newLimit) {
  TRI_ASSERT(_lock.isLockedWrite());
  TRI_ASSERT(metaGuard.isLocked());
  TRI_ASSERT(cache != nullptr);
  Metadata& metadata = cache->metadata();

  if (metadata.usage <= newLimit) {
    std::uint64_t oldLimit = metadata.hardUsageLimit;
    bool success = metadata.adjustLimits(newLimit, newLimit);
    TRI_ASSERT(success);
    metaGuard.release();

    if (newLimit != oldLimit) {
      TRI_ASSERT(_globalAllocation + newLimit - oldLimit >= _fixedAllocation);
      _globalAllocation -= oldLimit;
      _globalAllocation += newLimit;
      TRI_ASSERT(_globalAllocation >= _fixedAllocation);
    }
    return;
  }

  bool success = metadata.adjustLimits(newLimit, metadata.hardUsageLimit);
  TRI_ASSERT(success);
  TRI_ASSERT(!metadata.isResizing());
  metadata.toggleResizing();
  metaGuard.release();

  bool dispatched = false;
  if (!cache->isShutdown()) {
    try {
      auto task = std::make_shared<FreeMemoryTask>(environment, *this,
                                                   cache->shared_from_this());
      dispatched = task->dispatch();
    } catch (...) {
      dispatched = false;
    }
  }

  if (!dispatched) {
    SpinLocker altMetaGuard(SpinLocker::Mode::Write, metadata.lock());
    metadata.toggleResizing();
  }
}

void Manager::migrateCache(Manager::TaskEnvironment environment,
                           SpinLocker&& metaGuard, Cache* cache,
                           std::shared_ptr<Table> table) {
  TRI_ASSERT(_lock.isLockedWrite());
  TRI_ASSERT(metaGuard.isLocked());
  TRI_ASSERT(cache != nullptr);
  Metadata& metadata = cache->metadata();

  TRI_ASSERT(!metadata.isMigrating());
  metadata.toggleMigrating();
  TRI_ASSERT(metadata.isMigrating());
  metaGuard.release();

  bool dispatched = false;
  if (!cache->isShutdown()) {
    try {
      auto task = std::make_shared<MigrateTask>(
          environment, *this, cache->shared_from_this(), table);
      dispatched = task->dispatch();
    } catch (...) {
      dispatched = false;
    }
  }

  if (!dispatched) {
    SpinLocker altMetaGuard(SpinLocker::Mode::Write, metadata.lock());
    reclaimTable(std::move(table), true);
    TRI_ASSERT(metadata.isMigrating());
    metadata.toggleMigrating();
    TRI_ASSERT(!metadata.isMigrating());
  }
}

std::shared_ptr<Table> Manager::leaseTable(std::uint32_t logSize) {
  TRI_ASSERT(_lock.isLockedWrite());

  std::shared_ptr<Table> table;
  TRI_ASSERT(_tables.size() >= logSize);
  if (_tables[logSize].empty()) {
    if (increaseAllowed(Table::allocationSize(logSize), true)) {
      try {
        table = std::make_shared<Table>(logSize);

        _globalAllocation += table->memoryUsage();
        TRI_ASSERT(_globalAllocation >= _fixedAllocation);
        ++_activeTables;
      } catch (std::bad_alloc const&) {
        // don't throw from here, but return a nullptr
        table.reset();
      }
    }
  } else {
    table = std::move(_tables[logSize].top());
    _tables[logSize].pop();
    TRI_ASSERT(table != nullptr);
    _spareTableAllocation -= table->memoryUsage();
    TRI_ASSERT(_spareTables > 0);
    --_spareTables;
    ++_activeTables;
  }

  return table;
}

void Manager::reclaimTable(std::shared_ptr<Table>&& table, bool internal) {
  TRI_ASSERT(table != nullptr);

  // max table size to keep around empty is 32 MB
  constexpr std::uint64_t maxTableSize = std::uint64_t(32) << 20;

  {
    SpinLocker guard(SpinLocker::Mode::Write, _lock, !internal);

    TRI_ASSERT(_activeTables > 0);
    --_activeTables;

    std::uint64_t memoryUsage = table->memoryUsage();
    std::uint32_t logSize = table->logSize();
    std::size_t maxTables =
        (logSize < 18) ? (static_cast<std::size_t>(1) << (18 - logSize)) : 1;
    if ((_tables[logSize].size() < maxTables) &&
        (memoryUsage <= maxTableSize) &&
        (_spareTables < kMaxSpareTablesTotal) &&
        ((memoryUsage + _spareTableAllocation) <
         ((_globalSoftLimit - _globalHighwaterMark) / 2))) {
      _tables[logSize].emplace(std::move(table));
      _spareTableAllocation += memoryUsage;
      ++_spareTables;
      TRI_ASSERT(_spareTables <= kMaxSpareTablesTotal);
    } else {
      TRI_ASSERT(_globalAllocation >= memoryUsage + _fixedAllocation);
      _globalAllocation -= memoryUsage;
      TRI_ASSERT(_globalAllocation >= _fixedAllocation);
    }
  }

  // reclaim space outside of the lock
  table.reset();
}

bool Manager::increaseAllowed(std::uint64_t increase,
                              bool privileged) const noexcept {
  TRI_ASSERT(_lock.isLocked());
  if (privileged) {
    if (_resizing && (_globalAllocation <= _globalSoftLimit)) {
      return (increase <= (_globalSoftLimit - _globalAllocation));
    }

    return (increase <= (_globalHardLimit - _globalAllocation));
  }

  return (increase <= (_globalHighwaterMark - _globalAllocation));
}

std::shared_ptr<Manager::PriorityList> Manager::priorityList() {
  TRI_ASSERT(_lock.isLockedWrite());
  double minimumWeight = static_cast<double>(Manager::minCacheAllocation) /
                         static_cast<double>(_globalHighwaterMark);
  while (static_cast<std::uint64_t>(std::ceil(
             minimumWeight * static_cast<double>(_globalHighwaterMark))) <
         Manager::minCacheAllocation) {
    minimumWeight *= 1.001;  // bump by 0.1% until we fix precision issues
  }

  double uniformMarginalWeight = 0.2 / static_cast<double>(_caches.size());
  double baseWeight = std::max(minimumWeight, uniformMarginalWeight);

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  LOG_TOPIC("7eac8", DEBUG, Logger::CACHE)
      << "uniformMarginalWeight " << uniformMarginalWeight;
  LOG_TOPIC("108e6", DEBUG, Logger::CACHE) << "baseWeight " << baseWeight;
  if (1.0 < (baseWeight * static_cast<double>(_caches.size()))) {
    LOG_TOPIC("b2f55", FATAL, Logger::CACHE)
        << "weight: " << baseWeight << ", count: " << _caches.size();
    TRI_ASSERT(1.0 >= (baseWeight * static_cast<double>(_caches.size())));
  }
#endif
  double remainingWeight =
      1.0 - (baseWeight * static_cast<double>(_caches.size()));

  auto list = std::make_shared<PriorityList>();
  list->reserve(_caches.size());

  // catalog accessed caches and count total accesses
  // to get basis for comparison
  typename AccessStatBuffer::stats_t stats = _accessStats.getFrequencies();
  std::set<std::uint64_t> accessed;
  std::uint64_t totalAccesses = 0;
  std::uint64_t globalUsage = 0;
  for (auto const& s : stats) {
    auto c = _caches.find(s.first);
    if (c != _caches.end()) {
      totalAccesses += s.second;
      accessed.emplace(c->second->id());
    }
  }
  totalAccesses = std::max(static_cast<std::uint64_t>(1), totalAccesses);

  double allocFrac =
      0.8 * std::min(1.0, static_cast<double>(_globalAllocation) /
                              static_cast<double>(_globalHighwaterMark));
  // calculate global data usage
  for (auto it = _caches.begin(); it != _caches.end(); it++) {
    globalUsage += it->second->usage();
  }
  globalUsage = std::max(globalUsage,
                         static_cast<std::uint64_t>(1));  // avoid div-by-zero

  // gather all unaccessed caches at beginning of list
  for (auto it = _caches.begin(); it != _caches.end(); it++) {
    std::shared_ptr<Cache>& cache = it->second;
    auto found = accessed.find(cache->id());
    if (found == accessed.end()) {
      double weight = baseWeight + (cache->usage() / globalUsage) * allocFrac;
      list->emplace_back(cache, weight);
    }
  }

  double accessNormalizer = ((1.0 - allocFrac) * remainingWeight) /
                            static_cast<double>(totalAccesses);
  double usageNormalizer =
      (allocFrac * remainingWeight) / static_cast<double>(globalUsage);

  // gather all accessed caches in order
  for (auto s : stats) {
    auto it = accessed.find(s.first);
    if (it != accessed.end()) {
      std::shared_ptr<Cache>& cache = _caches.find(s.first)->second;
      double accessWeight = static_cast<double>(s.second) * accessNormalizer;
      double usageWeight =
          static_cast<double>(cache->usage()) * usageNormalizer;

      TRI_ASSERT(accessWeight >= 0.0);
      TRI_ASSERT(usageWeight >= 0.0);
      list->emplace_back(cache, (baseWeight + accessWeight + usageWeight));
    }
  }

  return list;
}

Manager::time_point Manager::futureTime(std::uint64_t millisecondsFromNow) {
  return (std::chrono::steady_clock::now() +
          std::chrono::milliseconds(millisecondsFromNow));
}

bool Manager::pastRebalancingGracePeriod() const {
  TRI_ASSERT(_lock.isLocked());
  bool ok = !_rebalancing;
  if (ok) {
    ok = (std::chrono::steady_clock::now() - _rebalanceCompleted) >=
         Manager::rebalancingGracePeriod;
  }

  return ok;
}

// template instantiations for createCache()
template std::shared_ptr<Cache> Manager::createCache<BinaryKeyHasher>(
    CacheType type, bool enableWindowedStats, std::uint64_t maxSize);

template std::shared_ptr<Cache> Manager::createCache<VPackKeyHasher>(
    CacheType type, bool enableWindowedStats, std::uint64_t maxSize);

}  // namespace arangodb::cache
