////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_STATISTICS_SERVER_STATISTICS_H
#define ARANGOD_STATISTICS_SERVER_STATISTICS_H 1

#include <cstdint>
#include "RestServer/Metrics.h"

namespace arangodb {
class MetricsFeature;

struct TransactionStatistics {
  explicit TransactionStatistics(arangodb::MetricsFeature&);
  TransactionStatistics(TransactionStatistics const&) = delete;
  TransactionStatistics(TransactionStatistics &&) = delete;
  TransactionStatistics& operator=(TransactionStatistics const&) = delete;
  TransactionStatistics& operator=(TransactionStatistics &&) = delete;

  arangodb::MetricsFeature& _metrics;
  
  Counter& _transactionsStarted;
  Counter& _transactionsAborted;
  Counter& _transactionsCommitted;
  Counter& _intermediateCommits;
  
  // total number of lock timeouts for exclusive locks
  Counter& _exclusiveLockTimeouts;
  // total number of lock timeouts for write locks
  Counter& _writeLockTimeouts;
  // total duration of lock acquisition (in microseconds)
  Counter& _lockTimeMicros;
  // histogram for lock acquisition (in seconds)
  Histogram<log_scale_t<double>>& _lockTimes;
  // Total number of times we used a fallback to sequential locking
  Counter& _sequentialLocks;
  
  // Total number of write operations in storage engine (excl. sync replication)
  Counter& _numWrites;
  // Total number of write operations in storage engine by sync replication
  Counter& _numWritesReplication;
  // Total number of truncate operations (not number of documents truncated!) (excl. sync replication)
  Counter& _numTruncates;
  // Total number of truncate operations (not number of documents truncated!) by sync replication
  Counter& _numTruncatesReplication;

  Histogram<log_scale_t<float>>& _rocksdb_read_usec;
  Histogram<log_scale_t<float>>& _rocksdb_insert_usec;
  Histogram<log_scale_t<float>>& _rocksdb_replace_usec;
  Histogram<log_scale_t<float>>& _rocksdb_remove_usec;
  Histogram<log_scale_t<float>>& _rocksdb_update_usec;
  Histogram<log_scale_t<float>>& _rocksdb_truncate_usec;
};

struct ServerStatistics {
  ServerStatistics(ServerStatistics const&) = delete;
  ServerStatistics(ServerStatistics &&) = delete;
  ServerStatistics& operator=(ServerStatistics const&) = delete;
  ServerStatistics& operator=(ServerStatistics &&) = delete;

  ServerStatistics& statistics();

  TransactionStatistics _transactionsStatistics;
  double const _startTime;
  
  double uptime() const noexcept;

  explicit ServerStatistics(arangodb::MetricsFeature& metrics, double start)
      : _transactionsStatistics(metrics), _startTime(start) {}
};

} // namespace

#endif
