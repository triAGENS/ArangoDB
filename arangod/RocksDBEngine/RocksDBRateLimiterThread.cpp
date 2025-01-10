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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBRateLimiterThread.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Logger/LogMacros.h"
#include "RocksDBEngine/RocksDBEngine.h"

#include <rocksdb/db.h>
#include <rocksdb/statistics.h>
#include <rocksdb/utilities/transaction_db.h>

// from original RocksDBThrottle.cpp implementation:
// NOT public rocksdb headers
// ugliness starts here ... this will go away if rocksdb adds pluggable
// write_controller.
#define ROCKSDB_PLATFORM_POSIX 1
#include <db/db_impl/db_impl.h>
#include <db/write_controller.h>

#include <limits>

using namespace arangodb;

namespace {
// rocksdb does not track flush time in its statistics. save start time
// in a thread-local storage.
// rocksdb flushes and compactions start and stop within same thread, so
// no overlapping can happen.
thread_local std::chrono::steady_clock::time_point flushStart =
    std::chrono::steady_clock::time_point{};
}  // namespace

RocksDBRateLimiterThread::RocksDBRateLimiterThread(
    RocksDBEngine& engine, uint64_t numSlots, uint64_t frequency,
    uint64_t scalingFactor, uint64_t minWriteRate, uint64_t maxWriteRate)
    : Thread(engine.server(), "RocksDBRateLimiter"),
      _engine(engine),
      _db(nullptr),
      _numSlots(numSlots),
      _frequency(frequency),
      _scalingFactor(scalingFactor),
      _minWriteRate(minWriteRate),
      _maxWriteRate(maxWriteRate),
      _currentRate(0),
      _currentHistoryIndex(0) {
  if (_maxWriteRate == 0) {
    _maxWriteRate = std::numeric_limits<decltype(_maxWriteRate)>::max();
  } else {
    _maxWriteRate = std::max(_maxWriteRate, _minWriteRate);
  }
  TRI_ASSERT(_minWriteRate <= _maxWriteRate);

  _history.resize(_numSlots);
}

RocksDBRateLimiterThread::~RocksDBRateLimiterThread() { shutdown(); }

void RocksDBRateLimiterThread::OnFlushBegin(
    rocksdb::DB* /*db*/, rocksdb::FlushJobInfo const& /*flush_job_info*/) {
  // save start time in thread-local storage
  ::flushStart = std::chrono::steady_clock::now();
}

void RocksDBRateLimiterThread::OnFlushCompleted(
    rocksdb::DB* db, rocksdb::FlushJobInfo const& flush_job_info) {
  // pick up flush start time from thread-local storage and calculate
  // duration
  std::chrono::microseconds flushTime =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - ::flushStart);
  uint64_t flushSize = flush_job_info.table_properties.data_size +
                       flush_job_info.table_properties.index_size +
                       flush_job_info.table_properties.filter_size;

  for (auto const& blob : flush_job_info.blob_file_addition_infos) {
    flushSize += blob.total_blob_bytes;
  }

  LOG_TOPIC("09fd4", TRACE, Logger::ENGINES)
      << "rocksdb flush completed. flush size: " << flushSize
      << ", micros: " << flushTime.count();

  // update values in current history slot
  std::lock_guard<std::mutex> guard{_mutex};
  size_t currentHistoryIndex = actualHistoryIndex();
  TRI_ASSERT(currentHistoryIndex < _history.size());
  _history[currentHistoryIndex].totalBytes += flushSize;
  _history[currentHistoryIndex].totalTime += flushTime;
  // intentionally do not adjust compaction debt here.
  // the compaction debt is stored only at the end of every
  // interval, querying the value from a RocksDB metric.
}

void RocksDBRateLimiterThread::OnCompactionCompleted(
    rocksdb::DB* db, rocksdb::CompactionJobInfo const& ci) {
  uint64_t rate = _currentRate.load(std::memory_order_relaxed);
  if (rate < _minWriteRate) {
    // rate was not yet set. let RocksDB figure out the initial write rates
    return;
  }
  // after a compaction has finished, set the write rate in RocksDB again.
  // this is necessary because RocksDB overrides the write rate we are
  // setting from the outside with its own values.

  // must acquire mutex here to avoid races in _db member in setRateInRocksDB
  std::unique_lock guard{_mutex};
  setRateInRocksDB(rate, "compaction completed");
}

void RocksDBRateLimiterThread::OnStallConditionsChanged(
    rocksdb::WriteStallInfo const& /*info*/) {
  uint64_t rate = _currentRate.load(std::memory_order_relaxed);
  if (rate < _minWriteRate) {
    // rate was not yet set. let RocksDB figure out the initial write rates
    return;
  }
  // after stall conditions changed, set the write rate in RocksDB again.
  // this is necessary because RocksDB overrides the write rate we are
  // setting from the outside with its own values.

  // must acquire mutex here to avoid races in _db member in setRateInRocksDB
  std::unique_lock guard{_mutex};
  setRateInRocksDB(rate, "stall conditions changed");
}

void RocksDBRateLimiterThread::beginShutdown() {
  Thread::beginShutdown();

  std::lock_guard guard{_mutex};
  _delayToken.reset();
  _cv.notify_all();
}

void RocksDBRateLimiterThread::run() {
  while (!isStopping()) {
    try {
      std::unique_lock guard{_mutex};

      // don't throttle while we are still in recovery or have not been
      // initialized properly
      if (!_engine.inRecovery() && !_families.empty()) {
        TRI_ASSERT(_db != nullptr);
        size_t currentHistoryIndex = actualHistoryIndex();

        // set the compaction debt value once for the current slot, using the
        // metrics provided by RocksDB. these include compaction debt for all
        // column families combined, including files on level 0.
        TRI_ASSERT(_history[currentHistoryIndex].compactionDebt == 0);
        _history[currentHistoryIndex].compactionDebt =
            computePendingCompactionBytes();

        // sum up all recorded values from history
        uint64_t totalSize = 0;
        std::chrono::microseconds totalTime{};
        uint64_t totalCompactionDebt = 0;
        size_t filledSlots = 0;
        size_t compactionSlots = 0;

        for (auto const& it : _history) {
          if (it.totalBytes > 0) {
            // take all slots into account in which flushes happened.
            totalSize += it.totalBytes;
            totalTime += it.totalTime;
            ++filledSlots;
          }

          // use compaction debt values from every slot, including those
          // for which there was not compaction debt. we will calculate an
          // average over the compaction debt in all slots, because the
          // compaction metrics produced by RocksDB is very volatile.
          totalCompactionDebt += it.compactionDebt;
          ++compactionSlots;
        }

        // if we have too few data points (e.g. less than 3) with flushes,
        // it is not good to use an average. only do the averaging and adjust
        // the write rate in case enough writes happened and we have enough
        // data to do the averages.
        if (filledSlots >= 3) {
          auto tc = totalTime.count();
          TRI_ASSERT(tc > 0);

          // target write rate based only on how much data was flushed.
          uint64_t targetRate = (totalSize * 1'000'000) / tc;

          // calculate average compaction debt, averaging the compaction debt
          // metrics from RocksDB over the entire history.
          uint64_t pendingCompactionBytes = 0;
          if (compactionSlots > 0) {
            pendingCompactionBytes = totalCompactionDebt / compactionSlots;
          }

          auto internalRocksDB = dynamic_cast<rocksdb::DBImpl*>(_db);
          TRI_ASSERT(internalRocksDB != nullptr);

          double percentReached = 0.0;
          if (uint64_t compactionHardLimit =
                  internalRocksDB->GetOptions()
                      .hard_pending_compaction_bytes_limit;
              compactionHardLimit > 0) {
            // if we are above 25% of the pending compaction bytes stop trigger,
            // take everything into account that is above this threshold, and
            // use it to slow down the writes.
            uint64_t threshold = compactionHardLimit / 4;
            if (pendingCompactionBytes > threshold) {
              // we are above the threshold, so penalize writes so that
              // compaction can keep up long-term. the closer we are to the stop
              // trigger, the more we will subtract from the target write rate.
              percentReached = std::min<double>(
                  0.99,
                  static_cast<double>(pendingCompactionBytes - threshold) /
                      static_cast<double>(compactionHardLimit - threshold));
              targetRate -= percentReached * targetRate;
            }
          }

          // fetch our old write rate
          uint64_t oldRate = _currentRate.load(std::memory_order_relaxed);
          // calculate the new write rate. to reduce volatility, the new
          // write rate will be based on the old write rate, and the
          // difference between target write rate and old write rate is
          // only applied gradually (using a scaling factor). the larger
          // the scaling factor is, the smoother the write rate adjustments
          // will be, but the slower the reaction to changes will be.
          uint64_t newRate = std::invoke([&]() {
            if (oldRate == 0) {
              // never had a write rate set. use our initially calculated
              // target write rate as the starting point.
              return targetRate;
            }
            if (targetRate > oldRate) {
              // increase write rate. for this used a reduced scaling factor
              // (scaling factor / 2), so that increases in the write rate
              // will be propagated more quickly than reductions.
              return oldRate + (targetRate - oldRate) / (_scalingFactor / 2);
            } else {
              // decreate write rate. for this use the original scaling factor.
              return oldRate - (oldRate - targetRate) / _scalingFactor;
            }
          });

          // write rate must always be between configured minimum and maximum
          // write rates.
          newRate = std::clamp(newRate, _minWriteRate, _maxWriteRate);

          LOG_TOPIC("37e36", INFO, Logger::ENGINES)
              << "rocksdb rate limiter total bytes flushed: " << totalSize
              << ", total micros: " << tc << ", target rate: " << targetRate
              << ", old rate: " << oldRate << ", new rate: " << newRate
              << ", rate diff: " << (int64_t(newRate) - int64_t(oldRate))
              << ", current compaction debt: "
              << _history[currentHistoryIndex].compactionDebt
              << ", average compaction debt: "
              << (compactionSlots > 0 ? (totalCompactionDebt / compactionSlots)
                                      : 0)
              << ", compaction stop trigger percent reached: "
              << (percentReached * 100.0) << "%";

          // update global rate
          _currentRate.store(newRate, std::memory_order_relaxed);

          setRateInRocksDB(newRate, "rate limiter calculation");
          if (_delayToken == nullptr) {
            // from original RocksDBThrottle.cpp implementation:
            // we directly access RocksDB's internal write_controller
            // here. this is technically not supported, but there is
            // no better way to set the write rate for RocksDB from
            // the outside.
            _delayToken =
                ((rocksdb::WriteController&)internalRocksDB->write_controller())
                    .GetDelayToken(newRate);
          }
        }

        // bump current history index. it is fine if this counter is larger than
        // the history size or even if it overflows.
        ++_currentHistoryIndex;
        currentHistoryIndex = actualHistoryIndex();

        // reset current slot for all things to come
        _history[currentHistoryIndex] = {
            .compactionDebt = 0, .totalBytes = 0, .totalTime = {}};
      }

      // wait until we are woken up
      _cv.wait_for(guard, std::chrono::milliseconds(_frequency));
    } catch (std::exception const& ex) {
      LOG_TOPIC("75584", WARN, Logger::ENGINES)
          << "caught exception in rocksdb rate limiter thread: " << ex.what();
    }
  }

  std::unique_lock guard{_mutex};
  _delayToken.reset();
}

uint64_t RocksDBRateLimiterThread::currentRate() const noexcept {
  return _currentRate.load(std::memory_order_relaxed);
}

size_t RocksDBRateLimiterThread::actualHistoryIndex() const noexcept {
  return _currentHistoryIndex % _history.size();
}

void RocksDBRateLimiterThread::setFamilies(
    std::vector<rocksdb::ColumnFamilyHandle*> const& families) {
  TRI_ASSERT(!families.empty());

  std::unique_lock guard{_mutex};
  TRI_ASSERT(_families.empty());
  _families = families;

  _db = _engine.db()->GetRootDB();
  TRI_ASSERT(_db != nullptr);
}

uint64_t RocksDBRateLimiterThread::computePendingCompactionBytes() const {
  TRI_ASSERT(_db != nullptr);
  int64_t pendingCompactionBytes = 0;

  // sum up the estimated compaction bytes for all column families.
  // the estimated compaction bytes in RocksDB are calculated when RocksDB
  // installs a new version (i.e. the set of .sst files changes).
  // the calculated value includes files on all levels, including level 0.
  std::string result;
  for (auto const& cf : _families) {
    if (_db->GetProperty(
            cf, rocksdb::DB::Properties::kEstimatePendingCompactionBytes,
            &result)) {
      try {
        pendingCompactionBytes += std::stoll(result);
      } catch (...) {
        // in theory, std::stoll can throw
      }
    }
  }
  return pendingCompactionBytes;
}

void RocksDBRateLimiterThread::setRateInRocksDB(uint64_t rate,
                                                std::string_view /*context*/) {
  // must be called with the mutex held.
  // note: context currently has no purpose, but can be used for manual
  // debugging
  if (_db == nullptr || rate < _minWriteRate) {
    return;
  }
  auto internalRocksDB = dynamic_cast<rocksdb::DBImpl*>(_db);
  // adjust write rate value in RocksDB. execute this under RocksDB's DB mutex.
  // these parts of RocksDB are normally not exposed publicly, so this is
  // quite a hack.
  rocksdb::InstrumentedMutexLock db_mutex(internalRocksDB->mutex());
  auto& writeController = const_cast<rocksdb::WriteController&>(
      internalRocksDB->write_controller());
  if (writeController.max_delayed_write_rate() < rate) {
    writeController.set_max_delayed_write_rate(rate);
  }
  writeController.set_delayed_write_rate(rate);
}
