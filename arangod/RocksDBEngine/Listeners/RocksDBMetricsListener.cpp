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
/// @author Matthew Von-Maszewski
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBMetricsListener.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/debugging.h"
#include "Logger/LogMacros.h"
#include "Metrics/CounterBuilder.h"
#include "Metrics/MetricsFeature.h"

DECLARE_COUNTER(
    arangodb_rocksdb_write_stalls_total,
    "Number of times RocksDB has entered a stalled (slowed) write state");
DECLARE_COUNTER(arangodb_rocksdb_write_stops_total,
                "Number of times RocksDB has entered a stopped write state");

namespace arangodb {

/// @brief Setup the object, clearing variables, but do no real work
RocksDBMetricsListener::RocksDBMetricsListener(ArangodServer& server)
    : _writeStalls(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_rocksdb_write_stalls_total{})),
      _writeStops(server.getFeature<metrics::MetricsFeature>().add(
          arangodb_rocksdb_write_stops_total{})) {}

void RocksDBMetricsListener::OnFlushBegin(rocksdb::DB*,
                                          rocksdb::FlushJobInfo const& info) {
  handleFlush("begin", info);
}

void RocksDBMetricsListener::OnFlushCompleted(
    rocksdb::DB*, rocksdb::FlushJobInfo const& info) {
  handleFlush("completed", info);
}

void RocksDBMetricsListener::OnCompactionBegin(
    rocksdb::DB*, rocksdb::CompactionJobInfo const& info) {
  handleCompaction("begin", info);
}

void RocksDBMetricsListener::OnCompactionCompleted(
    rocksdb::DB*, rocksdb::CompactionJobInfo const& info) {
  handleCompaction("completed", info);
}

void RocksDBMetricsListener::OnStallConditionsChanged(
    rocksdb::WriteStallInfo const& info) {
  // we should only get here if there's an actual change
  TRI_ASSERT(info.condition.cur != info.condition.prev);

  // in the case that we go from normal to stalled or stopped, we count it;
  // we also count a stall if we go from stopped to stall since it's a distinct
  // state

  if (info.condition.cur == rocksdb::WriteStallCondition::kDelayed) {
    _writeStalls.count();
    LOG_TOPIC("9123c", DEBUG, Logger::ENGINES)
        << "rocksdb is slowing incoming writes to column family '"
        << info.cf_name << "' to let background writes catch up";
  } else if (info.condition.cur == rocksdb::WriteStallCondition::kStopped) {
    _writeStops.count();
    LOG_TOPIC("9123d", WARN, Logger::ENGINES)
        << "rocksdb has stopped incoming writes to column family '"
        << info.cf_name << "' to let background writes catch up";
  } else {
    TRI_ASSERT(info.condition.cur == rocksdb::WriteStallCondition::kNormal);
    if (info.condition.prev == rocksdb::WriteStallCondition::kStopped) {
      LOG_TOPIC("9123e", INFO, Logger::ENGINES)
          << "rocksdb is resuming normal writes from stop for column family '"
          << info.cf_name << "'";
    } else {
      LOG_TOPIC("9123f", DEBUG, Logger::ENGINES)
          << "rocksdb is resuming normal writes from stall for column family '"
          << info.cf_name << "'";
    }
  }
}

void RocksDBMetricsListener::handleFlush(
    std::string_view phase, rocksdb::FlushJobInfo const& info) const {
  LOG_TOPIC("33d1f", DEBUG, Logger::ENGINES)
      << "rocksdb flush " << phase << " in column family " << info.cf_name
      << ", reason: " << rocksdb::GetFlushReasonString(info.flush_reason);
}

void RocksDBMetricsListener::handleCompaction(
    std::string_view phase, rocksdb::CompactionJobInfo const& info) const {
  LOG_TOPIC("1367c", DEBUG, Logger::ENGINES)
      << "rocksdb compaction " << phase << " in column family " << info.cf_name
      << " from base input level " << info.base_input_level
      << " to output level " << info.output_level
      << ", input files: " << info.input_files.size()
      << ", output files: " << info.output_files.size() << ", reason: "
      << rocksdb::GetCompactionReasonString(info.compaction_reason);
}

}  // namespace arangodb
