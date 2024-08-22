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

#include <memory>
#include "ReplicatedStateFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/Exceptions.tpp"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Logger/LogContextKeys.h"
#include "Logger/LogMacros.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedState/ReplicatedStateMetrics.h"
#include "Metrics/MetricsFeature.h"

using namespace arangodb;
using namespace arangodb::replication2;

auto replicated_state::ReplicatedStateFeature::createReplicatedState(
    std::string_view name, std::string_view database, LogId logId,
    std::shared_ptr<replicated_log::ReplicatedLog> log,
    LoggerContext const& loggerContext, std::shared_ptr<IScheduler> scheduler)
    -> std::shared_ptr<ReplicatedStateBase> {
  auto name_str = std::string{name};
  if (auto iter = implementations.find(name_str);
      iter != std::end(implementations)) {
    auto lc = loggerContext.with<logContextKeyStateImpl>(name_str)
                  .with<logContextKeyLogId>(logId);
    LOG_CTX("24af7", TRACE, lc)
        << "Creating replicated state of type `" << name << "`.";
    auto gid = GlobalLogIdentifier(std::string(database), logId);
    return iter->second.factory->createReplicatedState(
        std::move(gid), std::move(log), std::move(lc), iter->second.metrics,
        std::move(scheduler));
  }
  using namespace fmt::literals;
  throw basics::Exception::fmt(
      ADB_HERE, TRI_ERROR_REPLICATION_REPLICATED_STATE_IMPLEMENTATION_NOT_FOUND,
      "type"_a = name);
}

auto replicated_state::ReplicatedStateFeature::createReplicatedState(
    std::string_view name, std::string_view database, LogId logId,
    std::shared_ptr<replicated_log::ReplicatedLog> log,
    std::shared_ptr<IScheduler> scheduler)
    -> std::shared_ptr<ReplicatedStateBase> {
  return createReplicatedState(name, database, logId, std::move(log),
                               LoggerContext(Logger::REPLICATED_STATE),
                               std::move(scheduler));
}

void replicated_state::ReplicatedStateFeature::assertWasInserted(
    std::string_view name, bool wasInserted) {
  if (!wasInserted) {
    LOG_TOPIC("5b761", FATAL, Logger::REPLICATED_STATE)
        << "register state type with duplicated name " << name;
    FATAL_ERROR_EXIT();
  }
}

auto replicated_state::ReplicatedStateFeature::createMetricsObject(
    std::string_view name) -> std::shared_ptr<ReplicatedStateMetrics> {
  struct ReplicatedStateMetricsMock : ReplicatedStateMetrics {
    explicit ReplicatedStateMetricsMock(std::string_view name)
        : ReplicatedStateMetrics(nullptr, name) {}
  };

  return std::make_shared<ReplicatedStateMetricsMock>(name);
}
auto replicated_state::ReplicatedStateFeature::getDefaultStateOwnedMetadata(
    std::string_view name) -> storage::StateOwnedMetadata {
  auto name_str = std::string{name};
  if (auto iter = implementations.find(name_str);
      iter != std::end(implementations)) {
    return iter->second.factory->getDefaultStateOwnedMetadata();
  }
  using namespace fmt::literals;
  throw basics::Exception::fmt(
      ADB_HERE, TRI_ERROR_REPLICATION_REPLICATED_STATE_IMPLEMENTATION_NOT_FOUND,
      "type"_a = name);
}

replicated_state::ReplicatedStateAppFeature::ReplicatedStateAppFeature(
    Server& server)
    : ArangodFeature{server, *this} {
  setOptional(true);
  startsAfter<ReplicatedLogFeature>();
  onlyEnabledWith<ReplicatedLogFeature>();
}

auto replicated_state::ReplicatedStateAppFeature::createMetricsObject(
    std::string_view impl) -> std::shared_ptr<ReplicatedStateMetrics> {
  return std::make_shared<ReplicatedStateMetrics>(
      this->server().getFeature<metrics::MetricsFeature>(), impl);
}
