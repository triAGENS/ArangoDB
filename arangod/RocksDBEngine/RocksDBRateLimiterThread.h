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

#pragma once

#include "Basics/Thread.h"

#include <rocksdb/listener.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
struct CompactionJobInfo;
struct FlushJobInfo;
class WriteControllerToken;
}  // namespace rocksdb

namespace arangodb {
class RocksDBEngine;

// The purpose of this class is to serve as a rate limiter for write operations
// in RocksDB.
// Continuously writing data into RocksDB at a rate that is higher than what
// RocksDB's compaction can handle for a prolonged period will make RocksDB
// eventually run into full write stops. During a write stop no write in RocksDB
// except the compactions can make progress. Write stops cause arbitrarily long
// delays for user write operations, which can lead to confusion, and worse,
// timeouts when there are very long delays.
// So we want to avoid full write stops. That means we need RocksDB's compaction
// to keep up with the ingestion rate, at least on average.
// This class provides a rate-limiting mechanism for write operations based on
// the following steps:
// - Upon RocksDB start, a single instance of this class is created.
// - The class implements the rocksdb::EventListener interface, and we register
//   the single instance of this class as an event listener in RocksDB. Then we
//   subscribe to all RocksDB flush and compaction operations, and to all events
//   in which RocksDB changes the stall conditions for a column family. This
//   allows us to get notified about all flushes happening in RocksDB.
//   For flushes, we simply keep track of how much data was flushed and how
//   long the flush operation took. Both values are recorded in an array that
//   keeps track of the most recent flush operations, divided into various
//   time intervals.
// - The class also implements the Thread interface, so the single instance
//   also implements a background thread. This background thread wakes up in a
//   configurable interval (default: 1000ms) and then computes the average
//   write rate (all flush sizes in bytes divided by all flush durations) over
//   the array of historic values. It also stores the current compaction debt
//   (as reported by RocksDB) into the array for the current slot. It then seals
//   the current slot in the array, so that all following flush operations will
//   write to the next slot in the array. With the average values, it will
//   compute a new target write rate. Initially the target write rate is (number
//   of bytes flushed / flush duration).
//
//   We then check if the overall compaction debt (as reported by RocksDB) is
//   above a threshold that would justify throttling the write rate
//   artificially, so that pending compactions have a chance to keep up. We
//   currently start throttling the writes if the overall compaction debt is
//   greater than 25% of the compaction stop trigger. If the compaction debt
//   is higher than that, we compute how far we are away from the stop trigger.
//   The further we are still away from the stop trigger, the less we reduce
//   the targer rate. But the closer we are to the stop trigger, the more we
//   reduce the write rate.
//   The target write rate, potentially adjusted with the compaction debt
//   penalty, cannot simply be set as the new write rate, as it has no relation
//   to the previous write rate. Simply applying the new write rate could lead
//   to high volatily of write rates over time. To reduce volatility, we compute
//   the delta of the target write rate compared to the current write rate. The
//   delta is then added to or subtracted from the current write rate using a
//   scaling factor (e.g. 192). The scaling factor evens out large variations in
//   the delta over time. When the write rate gets increased, we use the
//   following formula:
//     new_write_rate = old_write_rate + delta / (scaling_factor / 2)
//   When the write rate gets decreased, we use the following formula:
//     new_write_rate = old_write_rate - delta / scaling_factor
//   That means increases of the write rate kick in earlier than decreases.
//
//   We then set this write rate in RocksDB as the "delayed_write_rate".
//
//   We are using an internal API of RocksDB to set the write rate, so this is
//   a bit of a hack. RocksDB may eventually overwrite the value we have set
//   with values it has computed on its own, clobbering our desired write rate.
//   Right now RocksDB does not provide a public API to set the write rate.
//   There is a RateLimiter API, but its main purpose seems to be to throttle
//   compactions so that they do not interfere too much with foreground write
//   activity. This is the opposite of what we want to achieve.
//
// - Whenever RocksDB finishes a compaction or changes its write stall
// conditions
//   internally, the rate limiter also gets notified, and simply installs the
//   already computed write rate in RocksDB again. This is necessary because
//   RocksDB may overwrite the write rate internally whenever it thinks it is
//   necessary. We simply need to overwrite it often enough with our own value.
//
// Notes on the computation of the target write rate:
//
// - We try to smooth out changes to the write rate to reduce the volatility.
//   The goal is to apply large changes to the the write rate gradually in
//   multiple smaller steps. We use the scaling factor to even out the deltas.
//   In addition, we keep track of the last x write rates and compaction debts,
//   and only use the averages over the recorded time period.
// - Increases of the write rate get a boost compared to decreases. We want
//   writes to resume quickly with higher throughput in case we have capacity
//   again.
// - The compaction debt reported by RocksDB is aggregated over all column
//   families. The value reported here is very volatile and can change
//   drastically in every period. We therefore also compute the average
//   compaction debt over the recorded time period. This helps to reduce
//   volatility as well.
// - RocksDB's own full write stop mechanisms work on a per-column family
//   basis. That means the compaction stop trigger value is checked by RocksDB
//   separately for each column family. As we have set the stop trigger to
//   a relatively high value (e.g. 16GB or higher), it is very unlikely that
//   RocksDB will build up that compaction debt inside a single column family.
//   Even if the compaction debt over all column families is higher than the
//   configured stop trigger, RocksDB will not care and only compare the
//   per-column family compaction debt against the stop trigger.
//   So we will likely never reach this and not run into a full write stop.
//   Instead, we try to compute a target write rate and tell RocksDB to
//   set the "delayed_write_rate" in a way so that we gradually reduce the
//   foreground writes before any of the column families can run into a
//   full write stop.
//   Our internal rate limiting starts once we reach 25% of the compaction
//   stop trigger value. The idea is that gradually slowing down writes once
//   we are above this threshold is enough to never reach the full compaction
//   stop trigger inside a single column family.
//
// Turning off our rate limiter has disastrous effects when running large
// scale ingestions, because the compactions will not be able to keep up with
// the ingestions over time, and eventually the compaction debt in one or
// multiple column families will be so large that we will run into full
// write stops.
//
// Once RocksDB provides a suitable API for setting the write rate, we can
// ditch our own rate limiter and simply use what RocksDB provides.
// But currently this is not the case.
class RocksDBRateLimiterThread final : public Thread,
                                       public rocksdb::EventListener {
 public:
  RocksDBRateLimiterThread(RocksDBEngine& engine, uint64_t numSlots,
                           uint64_t frequency, uint64_t scalingFactor,
                           uint64_t minWriteRate, uint64_t maxWriteRate);
  ~RocksDBRateLimiterThread();

  uint64_t currentRate() const noexcept;

  // overrides for listener interface from rocksdb/event_listener.h:
  void OnFlushBegin(rocksdb::DB* db,
                    rocksdb::FlushJobInfo const& flush_job_info) override;

  void OnFlushCompleted(rocksdb::DB* db,
                        rocksdb::FlushJobInfo const& flush_job_info) override;

  void OnCompactionCompleted(rocksdb::DB* db,
                             rocksdb::CompactionJobInfo const& /*ci*/) override;

  void OnStallConditionsChanged(
      rocksdb::WriteStallInfo const& /*info*/) override;

  void beginShutdown() override;

  void setFamilies(std::vector<rocksdb::ColumnFamilyHandle*> const& families);

 protected:
  void run() override;

 private:
  uint64_t computePendingCompactionBytes() const;
  // must be called with _mutex held
  size_t actualHistoryIndex() const noexcept;
  // must be called with _mutex held
  void setRateInRocksDB(uint64_t rate, std::string_view context);

  RocksDBEngine& _engine;

  rocksdb::DB* _db;

  std::vector<rocksdb::ColumnFamilyHandle*> _families;

  uint64_t _numSlots;
  uint64_t _frequency;  // provided in ms
  uint64_t _scalingFactor;
  uint64_t _minWriteRate;
  uint64_t _maxWriteRate;

  std::atomic<uint64_t> _currentRate;

  std::mutex _mutex;
  std::condition_variable _cv;

  // protected by _mutex
  struct HistoryEntry {
    uint64_t compactionDebt{0};
    uint64_t totalBytes{0};
    std::chrono::microseconds totalTime{};
  };
  std::vector<HistoryEntry> _history;
  // current rate limiting round. will start at 0 and will increase until
  // overflow. do not use it to access entries inside _history, as this will
  // produce out-of-bounds-accesses. instead use actualHistoryIndex() to
  // access the history items inside the valid range
  size_t _currentHistoryIndex;

  // protected by _mutex
  std::unique_ptr<rocksdb::WriteControllerToken> _delayToken;
};
}  // namespace arangodb
