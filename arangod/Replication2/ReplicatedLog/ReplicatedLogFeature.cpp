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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#include "ReplicatedLogFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/application-exit.h"
#include "ProgramOptions/Parameters.h"
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedLog/ReplicatedLogMetrics.h"
#include "Metrics/MetricsFeature.h"
#include "Logger/LogMacros.h"
#include "Cluster/ServerState.h"
#include "Basics/FeatureFlags.h"

#include <memory>

using namespace arangodb::options;
using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

ReplicatedLogFeature::ReplicatedLogFeature(Server& server)
    : ArangodFeature{server, *this},
      _options(std::make_shared<ReplicatedLogGlobalSettings>()) {
  static_assert(
      Server::isCreatedAfter<ReplicatedLogFeature, metrics::MetricsFeature>());

  setOptional(true);
  startsAfter<CommunicationFeaturePhase>();
  startsAfter<DatabaseFeaturePhase>();
}

auto ReplicatedLogFeature::metrics() const noexcept -> std::shared_ptr<
    replication2::replicated_log::ReplicatedLogMetrics> const& {
  return _replicatedLogMetrics;
}

void ReplicatedLogFeature::start() {
  _replicatedLogMetrics = std::make_shared<ReplicatedLogMetricsIndirect<false>>(
      &this->server().getFeature<metrics::MetricsFeature>());
}

auto ReplicatedLogFeature::options() const noexcept
    -> std::shared_ptr<replication2::ReplicatedLogGlobalSettings const> {
  return _options;
}

void ReplicatedLogFeature::prepare() {
  if (!::arangodb::replication2::EnableReplication2) {
    setEnabled(false);
    return;
  }
  if (ServerState::instance()->isCoordinator() ||
      ServerState::instance()->isAgent()) {
    setEnabled(false);
    return;
  }
}

void ReplicatedLogFeature::collectOptions(
    std::shared_ptr<ProgramOptions> options) {
#if defined(ARANGODB_ENABLE_MAINTAINER_MODE)
  options->addSection("replicated-log", "Options for replicated logs");

  options->addOption(
      "--replicated-log.threshold-network-batch-size",
      "send a batch of log updates early when threshold "
      "(in bytes) is exceeded",
      new SizeTParameter(
          &_options->_thresholdNetworkBatchSize, /*base*/ 1, /*minValue*/
          ReplicatedLogGlobalSettings::minThresholdNetworkBatchSize));
  options->addOption(
      "--replicated-log.threshold-rocksdb-write-batch-size",
      "write a batch of log updates to RocksDB early "
      "when threshold (in bytes) is exceeded",
      new SizeTParameter(
          &_options->_thresholdRocksDBWriteBatchSize, /*base*/ 1, /*minValue*/
          ReplicatedLogGlobalSettings::minThresholdRocksDBWriteBatchSize));
  options->addOption(
      "--replicated-log.threshold-log-compaction",
      "threshold for log compaction. Number of log entries to wait for before "
      "compacting.",
      new SizeTParameter(&_options->_thresholdLogCompaction, /*base*/ 1,
                         /*minValue*/ 0));
#endif
}

ReplicatedLogFeature::~ReplicatedLogFeature() = default;

#include "ReplicatedLogMetrics.tpp"

template struct arangodb::replication2::replicated_log::
    ReplicatedLogMetricsIndirect<false>;
