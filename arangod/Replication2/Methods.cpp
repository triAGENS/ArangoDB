////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include <Basics/Exceptions.h>
#include <Basics/voc-errors.h>

#include <yaclib/async/make.hpp>
#include <yaclib/coro/future.hpp>
#include <yaclib/coro/await.hpp>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/AgencyCallback.h"
#include "Cluster/AgencyCallbackRegistry.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Inspection/VPack.h"
#include "Network/Methods.h"
#include "Network/NetworkFeature.h"
#include "Replication2/AgencyMethods.h"
#include "Replication2/Exceptions/ParticipantResignedException.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/AgencySpecificationInspectors.h"
#include "Replication2/ReplicatedLog/LogLeader.h"
#include "Replication2/ReplicatedLog/LogStatus.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedState/ReplicatedState.h"
#include "VocBase/vocbase.h"

#include "Agency/AgencyPaths.h"
#include "Agency/AsyncAgencyComm.h"
#include "Methods.h"
#include "Random/RandomGenerator.h"

#include "Basics/Result.h"
#include "Basics/Result.tpp"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

namespace {
struct ReplicatedLogMethodsDBServer final
    : ReplicatedLogMethods,
      std::enable_shared_from_this<ReplicatedLogMethodsDBServer> {
  explicit ReplicatedLogMethodsDBServer(TRI_vocbase_t& vocbase)
      : vocbase(vocbase) {}
  auto waitForLogReady(LogId id, std::uint64_t version) const
      -> yaclib::Future<ResultT<consensus::index_t>> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto createReplicatedLog(CreateOptions spec) const
      -> yaclib::Future<ResultT<CreateResult>> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto createReplicatedLog(replication2::agency::LogTarget spec) const
      -> yaclib::Future<Result> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto deleteReplicatedLog(LogId id) const -> yaclib::Future<Result> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto getReplicatedLogs() const -> yaclib::Future<std::unordered_map<
      arangodb::replication2::LogId,
      std::variant<replicated_log::LogStatus, ParticipantsList>>> override {
    auto result = std::unordered_map<
        arangodb::replication2::LogId,
        std::variant<replicated_log::LogStatus, ParticipantsList>>{};
    for (auto& replicatedLog : vocbase.getReplicatedLogs()) {
      result[replicatedLog.first] = std::move(replicatedLog.second);
    }
    return yaclib::MakeFuture(std::move(result));
  }

  auto getLocalStatus(LogId id) const
      -> yaclib::Future<replication2::replicated_log::LogStatus> override {
    return yaclib::MakeFuture(
        vocbase.getReplicatedLogById(id)->getParticipant()->getStatus());
  }

  [[noreturn]] auto getGlobalStatus(
      LogId id, replicated_log::GlobalStatus::SpecificationSource) const
      -> yaclib::Future<replication2::replicated_log::GlobalStatus> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto getStatus(LogId id) const -> yaclib::Future<GenericLogStatus> override {
    return getLocalStatus(id).ThenInline(
        [](LogStatus&& status) { return GenericLogStatus(std::move(status)); });
  }

  auto getLogEntryByIndex(LogId id, LogIndex index) const
      -> yaclib::Future<std::optional<PersistingLogEntry>> override {
    auto entry = vocbase.getReplicatedLogById(id)
                     ->getParticipant()
                     ->copyInMemoryLog()
                     .getEntryByIndex(index);
    if (entry.has_value()) {
      return yaclib::MakeFuture<std::optional<PersistingLogEntry>>(
          entry->entry());
    } else {
      return yaclib::MakeFuture<std::optional<PersistingLogEntry>>(
          std::nullopt);
    }
  }

  auto slice(LogId id, LogIndex start, LogIndex stop) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    return yaclib::MakeFuture(vocbase.getReplicatedLogById(id)
                                  ->getParticipant()
                                  ->copyInMemoryLog()
                                  .getInternalIteratorRange(start, stop));
  }

  auto poll(LogId id, LogIndex index, std::size_t limit) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto leader = vocbase.getReplicatedLogLeaderById(id);
    return vocbase.getReplicatedLogById(id)
        ->getParticipant()
        ->waitFor(index)
        .ThenInline([index, limit, leader = std::move(leader),
                     self = shared_from_this()](WaitForResult&&)
                        -> std::unique_ptr<PersistedLogIterator> {
          auto log = leader->copyInMemoryLog();
          return log.getInternalIteratorRange(index, index + limit);
        });
  }

  auto tail(LogId id, std::size_t limit) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto log =
        vocbase.getReplicatedLogById(id)->getParticipant()->copyInMemoryLog();
    auto stop = log.getNextIndex();
    auto start = stop.saturatedDecrement(limit);
    return yaclib::MakeFuture(log.getInternalIteratorRange(start, stop));
  }

  auto head(LogId id, std::size_t limit) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto log =
        vocbase.getReplicatedLogById(id)->getParticipant()->copyInMemoryLog();
    auto start = log.getFirstIndex();
    return yaclib::MakeFuture(
        log.getInternalIteratorRange(start, start + limit));
  }

  auto insert(LogId id, LogPayload payload, bool waitForSync) const
      -> yaclib::Future<
          std::pair<LogIndex, replicated_log::WaitForResult>> override {
    auto log = vocbase.getReplicatedLogLeaderById(id);
    auto idx = log->insert(std::move(payload), waitForSync);
    return log->waitFor(idx).ThenInline([idx](WaitForResult&& result) {
      return std::make_pair(idx, std::move(result));
    });
  }

  auto insert(LogId id, TypedLogIterator<LogPayload>& iter,
              bool waitForSync) const
      -> yaclib::Future<std::pair<std::vector<LogIndex>,
                                  replicated_log::WaitForResult>> override {
    auto log = vocbase.getReplicatedLogLeaderById(id);
    auto indexes = std::vector<LogIndex>{};
    while (auto payload = iter.next()) {
      auto idx = log->insert(std::move(*payload));
      indexes.push_back(idx);
    }
    if (indexes.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "multi insert list must not be empty");
    }

    return log->waitFor(indexes.back())
        .ThenInline(
            [indexes = std::move(indexes)](WaitForResult&& result) mutable {
              return std::make_pair(std::move(indexes), std::move(result));
            });
  }

  auto insertWithoutCommit(LogId id, LogPayload payload, bool waitForSync) const
      -> yaclib::Future<LogIndex> override {
    auto log = vocbase.getReplicatedLogLeaderById(id);
    auto idx = log->insert(std::move(payload), waitForSync);
    return yaclib::MakeFuture(std::move(idx));
  }

  auto release(LogId id, LogIndex index) const
      -> yaclib::Future<Result> override {
    auto log = vocbase.getReplicatedLogById(id);
    return yaclib::MakeFuture(log->getParticipant()->release(index));
  }
  TRI_vocbase_t& vocbase;
};

namespace {
struct VPackLogIterator final : PersistedLogIterator {
  explicit VPackLogIterator(
      std::shared_ptr<velocypack::Buffer<uint8_t>> buffer_ptr)
      : buffer(std::move(buffer_ptr)),
        iter(VPackSlice(buffer->data()).get("result")),
        end(iter.end()) {}

  auto next() -> std::optional<PersistingLogEntry> override {
    while (iter != end) {
      return PersistingLogEntry::fromVelocyPack(*iter++);
    }
    return std::nullopt;
  }

 private:
  std::shared_ptr<velocypack::Buffer<uint8_t>> buffer;
  VPackArrayIterator iter;
  VPackArrayIterator end;
};

}  // namespace

struct ReplicatedLogMethodsCoordinator final
    : ReplicatedLogMethods,
      std::enable_shared_from_this<ReplicatedLogMethodsCoordinator> {
  auto waitForLogReady(LogId id, std::uint64_t version) const
      -> yaclib::Future<ResultT<consensus::index_t>> override {
    struct Context {
      explicit Context(uint64_t version) : version(version) {}
      yaclib::Promise<ResultT<consensus::index_t>> promise;
      std::uint64_t version;
    };

    auto ctx = std::make_shared<Context>(version);
    yaclib::Future<ResultT<consensus::index_t>> f;
    std::tie(f, ctx->promise) =
        yaclib::MakeContract<ResultT<consensus::index_t>>();

    using namespace cluster::paths;
    // register an agency callback and wait for the given version to appear in
    // target (or bigger)
    auto path = aliases::current()
                    ->replicatedLogs()
                    ->database(vocbase.name())
                    ->log(id)
                    ->supervision();
    auto cb = std::make_shared<AgencyCallback>(
        vocbase.server(), path->str(SkipComponents(1)),
        [ctx](velocypack::Slice slice, consensus::index_t index) -> bool {
          if (slice.isNone()) {
            return false;
          }

          auto supervision = velocypack::deserialize<
              replication2::agency::LogCurrentSupervision>(slice);
          if (supervision.targetVersion >= ctx->version) {
            std::move(ctx->promise).Set(ResultT<consensus::index_t>{index});
            return true;
          }
          return false;
        },
        true, true);

    if (auto result =
            clusterFeature.agencyCallbackRegistry()->registerCallback(cb, true);
        result.fail()) {
      return yaclib::MakeFuture<ResultT<consensus::index_t>>(std::move(result));
    }

    return std::move(f).ThenInline(
        [self = shared_from_this(), cb](auto&& result) {
          self->clusterFeature.agencyCallbackRegistry()->unregisterCallback(cb);
          return std::move(result).Ok();
        });
  }

  void fillCreateOptions(CreateOptions& options) const {
    if (!options.id.has_value()) {
      options.id = LogId{clusterInfo.uniqid()};
    }

    auto dbservers = clusterInfo.getCurrentDBServers();

    auto expectedNumberOfServers = std::min(dbservers.size(), std::size_t{3});
    if (options.numberOfServers.has_value()) {
      expectedNumberOfServers = *options.numberOfServers;
    } else if (!options.servers.empty()) {
      expectedNumberOfServers = options.servers.size();
    }

    if (!options.config.has_value()) {
      options.config = arangodb::replication2::agency::LogTargetConfig{
          2, expectedNumberOfServers, false};
    }

    if (expectedNumberOfServers > dbservers.size()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_CLUSTER_INSUFFICIENT_DBSERVERS);
    }

    // always make sure that the wished leader is part of the set of servers
    if (options.leader) {
      if (auto iter = std::find(options.servers.begin(), options.servers.end(),
                                *options.leader);
          iter == options.servers.end()) {
        options.servers.emplace_back(*options.leader);
      }
    }

    if (options.servers.size() < expectedNumberOfServers) {
      auto newEnd = dbservers.end();
      if (!options.servers.empty()) {
        newEnd = std::remove_if(
            dbservers.begin(), dbservers.end(),
            [&](ParticipantId const& server) {
              return std::find(options.servers.begin(), options.servers.end(),
                               server) != options.servers.end();
            });
      }

      std::shuffle(dbservers.begin(), newEnd,
                   RandomGenerator::UniformRandomGenerator<std::uint32_t>{});
      std::copy_n(dbservers.begin(),
                  expectedNumberOfServers - options.servers.size(),
                  std::back_inserter(options.servers));
    }
  }

  static auto createTargetFromCreateOptions(CreateOptions const& options)
      -> replication2::agency::LogTarget {
    replication2::agency::LogTarget target;
    target.id = options.id.value();
    target.config = options.config.value();
    target.leader = options.leader;
    target.version = 1;
    for (auto const& server : options.servers) {
      target.participants[server];
    }
    return target;
  }

  auto createReplicatedLog(CreateOptions options) const
      -> yaclib::Future<ResultT<CreateResult>> override {
    fillCreateOptions(options);
    TRI_ASSERT(options.id.has_value());
    auto target = createTargetFromCreateOptions(options);

    return createReplicatedLog(std::move(target))
        .ThenInline([options = std::move(options),
                     self = shared_from_this()](Result&& result) mutable
                    -> yaclib::Future<ResultT<CreateResult>> {
          auto response = CreateResult{*options.id, std::move(options.servers)};
          if (!result.ok()) {
            return yaclib::MakeFuture<ResultT<CreateResult>>(std::move(result));
          }

          if (options.waitForReady) {
            // wait for the state to be ready
            return self->waitForLogReady(*options.id, 1)
                .ThenInline([self, resp = std::move(response)](
                                ResultT<consensus::index_t>&& result) mutable
                            -> yaclib::Future<ResultT<CreateResult>> {
                  if (result.fail()) {
                    return yaclib::MakeFuture<ResultT<CreateResult>>(
                        result.result());
                  }
                  return self->clusterInfo
                      .fetchAndWaitForPlanVersion(std::chrono::seconds{240})
                      .ThenInline(
                          [resp = std::move(resp)](Result&& result) mutable
                          -> ResultT<CreateResult> {
                            if (result.fail()) {
                              return {result};
                            }
                            return std::move(resp);
                          });
                });
          }
          return yaclib::MakeFuture<ResultT<CreateResult>>(std::move(response));
        });
  }

  auto createReplicatedLog(replication2::agency::LogTarget spec) const
      -> yaclib::Future<Result> override {
    return replication2::agency::methods::createReplicatedLog(vocbase.name(),
                                                              spec)
        .ThenInline([self = shared_from_this()](
                        ResultT<uint64_t>&& res) -> yaclib::Future<Result> {
          if (res.fail()) {
            return yaclib::MakeFuture<Result>(res.result());
          }

          return self->clusterInfo.waitForPlan(res.get());
        });
  }

  auto deleteReplicatedLog(LogId id) const -> yaclib::Future<Result> override {
    return replication2::agency::methods::deleteReplicatedLog(vocbase.name(),
                                                              id)
        .ThenInline([self = shared_from_this()](
                        ResultT<uint64_t>&& res) -> yaclib::Future<Result> {
          if (res.fail()) {
            return yaclib::MakeFuture<Result>(res.result());
          }

          return self->clusterInfo.waitForPlan(res.get());
        });
  }

  auto getReplicatedLogs() const -> yaclib::Future<std::unordered_map<
      arangodb::replication2::LogId,
      std::variant<replicated_log::LogStatus, ParticipantsList>>> override {
    auto logsParticipants =
        clusterInfo.getReplicatedLogsParticipants(vocbase.name());

    if (logsParticipants.fail()) {
      THROW_ARANGO_EXCEPTION(logsParticipants.result());
    }

    auto result = std::unordered_map<
        arangodb::replication2::LogId,
        std::variant<replicated_log::LogStatus, ParticipantsList>>{};
    for (auto& replicatedLog : logsParticipants.get()) {
      result[replicatedLog.first] = std::move(replicatedLog.second);
    }
    return yaclib::MakeFuture(std::move(result));
  }

  [[noreturn]] auto getLocalStatus(LogId id) const
      -> yaclib::Future<replication2::replicated_log::LogStatus> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto getGlobalStatus(
      LogId id, replicated_log::GlobalStatus::SpecificationSource source) const
      -> yaclib::Future<replication2::replicated_log::GlobalStatus> override {
    // 1. Determine which source to use for gathering information
    // 2. Query information from all sources
    auto futureSpec = loadLogSpecification(vocbase.name(), id, source);
    return std::move(futureSpec)
        .ThenInline([self = shared_from_this(), source](
                        ResultT<std::shared_ptr<
                            replication2::agency::LogPlanSpecification const>>
                            result) {
          if (result.fail()) {
            THROW_ARANGO_EXCEPTION(result.result());
          }

          auto const& spec = result.get();
          TRI_ASSERT(spec != nullptr);
          return self->collectGlobalStatusUsingSpec(spec, source);
        });
  }

  auto getStatus(LogId id) const -> yaclib::Future<GenericLogStatus> override {
    return getGlobalStatus(id, GlobalStatus::SpecificationSource::kRemoteAgency)
        .ThenInline([](GlobalStatus&& status) {
          return GenericLogStatus(std::move(status));
        });
  }

  auto getLogEntryByIndex(LogId id, LogIndex index) const
      -> yaclib::Future<std::optional<PersistingLogEntry>> override {
    auto path =
        basics::StringUtils::joinT("/", "_api/log", id, "entry", index.value);
    network::RequestOptions opts;
    opts.database = vocbase.name();
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .ThenInline([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          auto entry =
              PersistingLogEntry::fromVelocyPack(resp.slice().get("result"));
          return std::optional<PersistingLogEntry>(std::move(entry));
        });
  }

  auto slice(LogId id, LogIndex start, LogIndex stop) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "slice");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["start"] = to_string(start);
    opts.parameters["stop"] = to_string(stop);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .ThenInline([](network::Response&& resp)
                        -> std::unique_ptr<PersistedLogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(
              resp.response().stealPayload());
        });
  }

  auto poll(LogId id, LogIndex index, std::size_t limit) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "poll");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["first"] = to_string(index);
    opts.parameters["limit"] = std::to_string(limit);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .ThenInline([](network::Response&& resp)
                        -> std::unique_ptr<PersistedLogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(
              resp.response().stealPayload());
        });
  }

  auto tail(LogId id, std::size_t limit) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "tail");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["limit"] = std::to_string(limit);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .ThenInline([](network::Response&& resp)
                        -> std::unique_ptr<PersistedLogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(
              resp.response().stealPayload());
        });
  }

  auto head(LogId id, std::size_t limit) const
      -> yaclib::Future<std::unique_ptr<PersistedLogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "head");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["limit"] = std::to_string(limit);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .ThenInline([](network::Response&& resp)
                        -> std::unique_ptr<PersistedLogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(
              resp.response().stealPayload());
        });
  }

  auto insert(LogId id, LogPayload payload, bool waitForSync) const
      -> yaclib::Future<
          std::pair<LogIndex, replicated_log::WaitForResult>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "insert");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.param(StaticStrings::WaitForSyncString,
               waitForSync ? "true" : "false");
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path,
                                payload.copyBuffer(), opts)
        .ThenInline([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          auto result = resp.slice().get("result");
          auto waitResult = result.get("result");

          auto quorum =
              std::make_shared<replication2::replicated_log::QuorumData const>(
                  waitResult.get("quorum"));
          auto commitIndex = waitResult.get("commitIndex").extract<LogIndex>();
          auto index = result.get("index").extract<LogIndex>();
          return std::make_pair(index, replicated_log::WaitForResult(
                                           commitIndex, std::move(quorum)));
        });
  }

  auto insert(LogId id, TypedLogIterator<LogPayload>& iter,
              bool waitForSync) const
      -> yaclib::Future<std::pair<std::vector<LogIndex>,
                                  replicated_log::WaitForResult>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "multi-insert");

    std::size_t payloadSize{0};
    VPackBuilder builder{};
    {
      VPackArrayBuilder arrayBuilder{&builder};
      while (auto const payload = iter.next()) {
        builder.add(payload->slice());
        ++payloadSize;
      }
    }

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.param(StaticStrings::WaitForSyncString,
               waitForSync ? "true" : "false");
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path,
                                builder.bufferRef(), opts)
        .ThenInline([payloadSize](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          auto result = resp.slice().get("result");
          auto waitResult = result.get("result");

          auto quorum =
              std::make_shared<replication2::replicated_log::QuorumData const>(
                  waitResult.get("quorum"));
          auto commitIndex = waitResult.get("commitIndex").extract<LogIndex>();

          std::vector<LogIndex> indexes;
          indexes.reserve(payloadSize);
          auto indexIter = velocypack::ArrayIterator(result.get("indexes"));
          std::transform(
              indexIter.begin(), indexIter.end(), std::back_inserter(indexes),
              [](auto const& it) { return it.template extract<LogIndex>(); });
          return std::make_pair(
              std::move(indexes),
              replicated_log::WaitForResult(commitIndex, std::move(quorum)));
        });
  }

  auto insertWithoutCommit(LogId id, LogPayload payload, bool waitForSync) const
      -> yaclib::Future<LogIndex> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "insert");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.param(StaticStrings::WaitForSyncString,
               waitForSync ? "true" : "false");
    opts.param(StaticStrings::DontWaitForCommit, "true");
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path,
                                payload.copyBuffer(), opts)
        .ThenInline([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          auto result = resp.slice().get("result");
          auto index = result.get("index").extract<LogIndex>();
          return index;
        });
  }

  auto release(LogId id, LogIndex index) const
      -> yaclib::Future<Result> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "release");

    VPackBufferUInt8 body;
    {
      VPackBuilder builder(body);
      builder.add(VPackSlice::emptyObjectSlice());
    }

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["index"] = to_string(index);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path, std::move(body),
                                opts)
        .ThenInline(
            [](network::Response&& resp) { return resp.combinedResult(); });
  }

  explicit ReplicatedLogMethodsCoordinator(TRI_vocbase_t& vocbase)
      : vocbase(vocbase),
        clusterFeature(vocbase.server().getFeature<ClusterFeature>()),
        clusterInfo(clusterFeature.clusterInfo()),
        pool(vocbase.server().getFeature<NetworkFeature>().pool()) {}

 private:
  auto getLogLeader(LogId id) const -> ServerID {
    auto leader = clusterInfo.getReplicatedLogLeader(id);
    if (leader.fail()) {
      if (leader.is(TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED)) {
        throw ParticipantResignedException(leader.result(), ADB_HERE);
      } else {
        THROW_ARANGO_EXCEPTION(leader.result());
      }
    }

    return *leader;
  }

  auto loadLogSpecification(DatabaseID const& database, replication2::LogId id,
                            GlobalStatus::SpecificationSource source) const
      -> yaclib::Future<ResultT<std::shared_ptr<
          arangodb::replication2::agency::LogPlanSpecification const>>> {
    if (source == GlobalStatus::SpecificationSource::kLocalCache) {
      return yaclib::MakeFuture(
          clusterInfo.getReplicatedLogPlanSpecification(id));
    } else {
      AsyncAgencyComm ac;
      auto f = ac.getValues(arangodb::cluster::paths::aliases::plan()
                                ->replicatedLogs()
                                ->database(database)
                                ->log(id),
                            std::chrono::seconds{5});

      return std::move(f).ThenInline(
          [self = shared_from_this(), id](auto&& tryResult)
              -> ResultT<std::shared_ptr<
                  arangodb::replication2::agency::LogPlanSpecification const>> {
            auto result = basics::catchToResultT(
                [&] { return std::move(tryResult).Ok(); });

            if (result.fail()) {
              return result.result();
            }

            if (result->value().isNone()) {
              return Result::fmt(TRI_ERROR_REPLICATION_REPLICATED_LOG_NOT_FOUND,
                                 id);
            }

            auto spec = velocypack::deserialize<
                arangodb::replication2::agency::LogPlanSpecification>(
                result->value());

            return {std::make_shared<
                arangodb::replication2::agency::LogPlanSpecification>(
                std::move(spec))};
          });
    }
  }

  auto readSupervisionStatus(replication2::LogId id) const
      -> yaclib::Future<GlobalStatus::SupervisionStatus> {
    AsyncAgencyComm ac;
    using Status = GlobalStatus::SupervisionStatus;
    // TODO move this into the agency methods
    auto f = ac.getValues(arangodb::cluster::paths::aliases::current()
                              ->replicatedLogs()
                              ->database(vocbase.name())
                              ->log(id)
                              ->supervision(),
                          std::chrono::seconds{5});
    return std::move(f).ThenInline(
        [self = shared_from_this()](auto&& tryResult) {
          auto result =
              basics::catchToResultT([&] { return std::move(tryResult).Ok(); });

          auto const statusFromResult = [](Result const& res) {
            return Status{
                .connection = {.error = res.errorNumber(),
                               .errorMessage = std::string{res.errorMessage()}},
                .response = std::nullopt};
          };

          if (result.fail()) {
            return statusFromResult(result.result());
          }
          auto& read = result.get();
          auto status = statusFromResult(read.asResult());
          if (read.ok() && !read.value().isNone()) {
            status.response.emplace(
                velocypack::deserialize<
                    arangodb::replication2::agency::LogCurrentSupervision>(
                    read.value()));
          }

          return status;
        });
  }

  auto queryParticipantsStatus(replication2::LogId id,
                               replication2::ParticipantId const& participant)
      const -> yaclib::Future<GlobalStatus::ParticipantStatus> {
    using Status = GlobalStatus::ParticipantStatus;
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "local-status");
    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.timeout = std::chrono::seconds{5};
    return network::sendRequest(pool, "server:" + participant,
                                fuerte::RestVerb::Get, path, {}, opts)
        .ThenInline([](auto&& tryResult) mutable {
          auto result =
              basics::catchToResultT([&] { return std::move(tryResult).Ok(); });

          auto const statusFromResult = [](Result const& res) {
            return Status{
                .connection = {.error = res.errorNumber(),
                               .errorMessage = std::string{res.errorMessage()}},
                .response = std::nullopt};
          };

          if (result.fail()) {
            return statusFromResult(result.result());
          }

          auto& response = result.get();
          auto status = statusFromResult(response.combinedResult());
          if (response.combinedResult().ok()) {
            status.response = GlobalStatus::ParticipantStatus::Response{
                .value =
                    replication2::replicated_log::LogStatus::fromVelocyPack(
                        response.slice().get("result"))};
          }
          return status;
        });
  }

  auto collectGlobalStatusUsingSpec(
      std::shared_ptr<replication2::agency::LogPlanSpecification const> spec,
      GlobalStatus::SpecificationSource source) const
      -> yaclib::Future<GlobalStatus> {
    std::vector<yaclib::Future<GlobalStatus::ParticipantStatus>> pfs;
    {
      auto const& participants = spec->participantsConfig.participants;
      pfs.reserve(participants.size());
      for (auto const& [id, flags] : participants) {
        pfs.emplace_back(queryParticipantsStatus(spec->id, id));
      }
    }
    auto af = readSupervisionStatus(spec->id);

    co_await yaclib::Await(pfs.begin(), pfs.end());
    co_await yaclib::Await(af);

    auto& agency = std::as_const(af).Touch().Ok();

    auto leader = std::optional<ParticipantId>{};
    if (spec->currentTerm && spec->currentTerm->leader) {
      leader = spec->currentTerm->leader->serverId;
    }
    auto participantsMap =
        std::unordered_map<ParticipantId, GlobalStatus::ParticipantStatus>{};

    auto const& participants = spec->participantsConfig.participants;
    std::size_t idx = 0;
    for (auto const& [id, flags] : participants) {
      participantsMap[id] = std::move(pfs.at(idx++)).Touch().Ok();
    }

    co_return GlobalStatus{.supervision = agency,
                           .participants = participantsMap,
                           .specification = {.source = source, .plan = *spec},
                           .leaderId = std::move(leader)};
  }

  TRI_vocbase_t& vocbase;
  ClusterFeature& clusterFeature;
  ClusterInfo& clusterInfo;
  network::ConnectionPool* pool;
};

struct ReplicatedStateDBServerMethods
    : std::enable_shared_from_this<ReplicatedStateDBServerMethods>,
      ReplicatedStateMethods {
  explicit ReplicatedStateDBServerMethods(TRI_vocbase_t& vocbase)
      : vocbase(vocbase) {}

  auto createReplicatedState(replicated_state::agency::Target spec) const
      -> yaclib::Future<Result> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_NOT_IMPLEMENTED);
  }

  auto deleteReplicatedState(LogId id) const
      -> yaclib::Future<Result> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_NOT_IMPLEMENTED);
  }

  [[nodiscard]] auto waitForStateReady(LogId, std::uint64_t)
      -> yaclib::Future<ResultT<consensus::index_t>> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto getLocalStatus(LogId id) const
      -> yaclib::Future<replicated_state::StateStatus> override {
    auto state = vocbase.getReplicatedStateById(id);
    if (auto status = state->getStatus(); status.has_value()) {
      return yaclib::MakeFuture(std::move(*status));
    }
    THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_NOT_IMPLEMENTED);
  }

  auto replaceParticipant(LogId logId, ParticipantId const& participantToRemove,
                          ParticipantId const& participantToAdd,
                          std::optional<ParticipantId> const& currentLeader)
      const -> yaclib::Future<Result> override {
    // Only available on the coordinator
    THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_NOT_IMPLEMENTED);
  }

  auto setLeader(LogId id, std::optional<ParticipantId> const& leaderId) const
      -> yaclib::Future<Result> override {
    // Only available on the coordinator
    THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_NOT_IMPLEMENTED);
  }

  auto getGlobalSnapshotStatus(LogId) const
      -> yaclib::Future<ResultT<GlobalSnapshotStatus>> override {
    // Only available on the coordinator
    THROW_ARANGO_EXCEPTION(TRI_ERROR_HTTP_NOT_IMPLEMENTED);
  }

  TRI_vocbase_t& vocbase;
};

struct ReplicatedStateCoordinatorMethods
    : std::enable_shared_from_this<ReplicatedStateCoordinatorMethods>,
      ReplicatedStateMethods {
  explicit ReplicatedStateCoordinatorMethods(ArangodServer& server,
                                             std::string databaseName)
      : server(server),
        clusterFeature(server.getFeature<ClusterFeature>()),
        clusterInfo(clusterFeature.clusterInfo()),
        databaseName(std::move(databaseName)) {}

  auto createReplicatedState(replicated_state::agency::Target spec) const
      -> yaclib::Future<Result> override {
    return replication2::agency::methods::createReplicatedState(databaseName,
                                                                spec)
        .ThenInline([self = shared_from_this()](
                        ResultT<uint64_t>&& res) -> yaclib::Future<Result> {
          if (res.fail()) {
            return yaclib::MakeFuture<Result>(res.result());
          }
          return self->clusterInfo.waitForPlan(res.get());
        });
  }

  [[nodiscard]] virtual auto waitForStateReady(LogId id, std::uint64_t version)
      -> yaclib::Future<ResultT<consensus::index_t>> override {
    struct Context {
      explicit Context(uint64_t version) : version(version) {}
      yaclib::Promise<ResultT<consensus::index_t>> promise;
      std::uint64_t version;
    };

    auto ctx = std::make_shared<Context>(version);
    yaclib::Future<ResultT<consensus::index_t>> f;
    std::tie(f, ctx->promise) =
        yaclib::MakeContract<ResultT<consensus::index_t>>();

    using namespace cluster::paths;
    // register an agency callback and wait for the given version to appear in
    // target (or bigger)
    auto path = aliases::current()
                    ->replicatedStates()
                    ->database(databaseName)
                    ->state(id)
                    ->supervision();
    auto cb = std::make_shared<AgencyCallback>(
        server, path->str(SkipComponents(1)),
        [ctx](velocypack::Slice slice, consensus::index_t index) -> bool {
          if (slice.isNone()) {
            return false;
          }

          auto supervision = velocypack::deserialize<
              replicated_state::agency::Current::Supervision>(slice);
          if (supervision.version.has_value() &&
              supervision.version >= ctx->version) {
            std::move(ctx->promise).Set(ResultT<consensus::index_t>{index});
            return true;
          }
          return false;
        },
        true, true);
    if (auto result =
            clusterFeature.agencyCallbackRegistry()->registerCallback(cb, true);
        result.fail()) {
      return yaclib::MakeFuture<ResultT<consensus::index_t>>(result);
    }

    return std::move(f).ThenInline(
        [self = shared_from_this(), cb](auto&& result) {
          self->clusterFeature.agencyCallbackRegistry()->unregisterCallback(cb);
          return std::move(result).Ok();
        });
  }

  auto deleteReplicatedState(LogId id) const
      -> yaclib::Future<Result> override {
    return replication2::agency::methods::deleteReplicatedState(databaseName,
                                                                id)
        .ThenInline([self = shared_from_this()](
                        ResultT<uint64_t>&& res) -> yaclib::Future<Result> {
          if (res.fail()) {
            return yaclib::MakeFuture<Result>(res.result());
          }

          return self->clusterInfo.waitForPlan(res.get());
        });
  }

  auto getLocalStatus(LogId id) const
      -> yaclib::Future<replicated_state::StateStatus> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto replaceParticipant(LogId id, ParticipantId const& participantToRemove,
                          ParticipantId const& participantToAdd,
                          std::optional<ParticipantId> const& currentLeader)
      const -> yaclib::Future<Result> override {
    return replication2::agency::methods::replaceReplicatedStateParticipant(
        databaseName, id, participantToRemove, participantToAdd, currentLeader);
  }

  auto setLeader(LogId id, std::optional<ParticipantId> const& leaderId) const
      -> yaclib::Future<Result> override {
    return replication2::agency::methods::replaceReplicatedSetLeader(
        databaseName, id, leaderId);
  }

  auto getGlobalSnapshotStatus(LogId id) const
      -> yaclib::Future<ResultT<GlobalSnapshotStatus>> override {
    AsyncAgencyComm ac;
    auto f = ac.getValues(arangodb::cluster::paths::aliases::current()
                              ->replicatedStates()
                              ->database(databaseName)
                              ->state(id),
                          std::chrono::seconds{5});
    return std::move(f).ThenInline(
        [self = shared_from_this(),
         id](auto&& tryResult) -> ResultT<GlobalSnapshotStatus> {
          auto result =
              basics::catchToResultT([&] { return std::move(tryResult).Ok(); });

          if (result.fail()) {
            return result.result();
          }
          if (result->value().isNone()) {
            return Result::fmt(TRI_ERROR_REPLICATION_REPLICATED_LOG_NOT_FOUND,
                               id.id());
          }
          auto current =
              velocypack::deserialize<replicated_state::agency::Current>(
                  result->value());

          GlobalSnapshotStatus status;
          for (auto const& [p, s] : current.participants) {
            status[p] = ParticipantSnapshotStatus{.status = s.snapshot,
                                                  .generation = s.generation};
          }

          return status;
        });
  }

  ArangodServer& server;
  ClusterFeature& clusterFeature;
  ClusterInfo& clusterInfo;
  std::string const databaseName;
};

}  // namespace

auto ReplicatedLogMethods::createInstance(TRI_vocbase_t& vocbase)
    -> std::shared_ptr<ReplicatedLogMethods> {
  switch (ServerState::instance()->getRole()) {
    case ServerState::ROLE_COORDINATOR:
      return std::make_shared<ReplicatedLogMethodsCoordinator>(vocbase);
    case ServerState::ROLE_DBSERVER:
      return std::make_shared<ReplicatedLogMethodsDBServer>(vocbase);
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "api only on available coordinators or dbservers");
  }
}

auto ReplicatedStateMethods::createInstance(TRI_vocbase_t& vocbase)
    -> std::shared_ptr<ReplicatedStateMethods> {
  switch (ServerState::instance()->getRole()) {
    case ServerState::ROLE_DBSERVER:
      return createInstanceDBServer(vocbase);
    case ServerState::ROLE_COORDINATOR:
      return createInstanceCoordinator(vocbase.server(), vocbase.name());
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "api only on available coordinators or dbservers");
  }
}

auto ReplicatedStateMethods::createInstanceDBServer(TRI_vocbase_t& vocbase)
    -> std::shared_ptr<ReplicatedStateMethods> {
  ADB_PROD_ASSERT(ServerState::instance()->getRole() ==
                  ServerState::ROLE_DBSERVER);
  return std::make_shared<ReplicatedStateDBServerMethods>(vocbase);
}

auto ReplicatedStateMethods::createInstanceCoordinator(ArangodServer& server,
                                                       std::string databaseName)
    -> std::shared_ptr<ReplicatedStateMethods> {
  ADB_PROD_ASSERT(ServerState::instance()->getRole() ==
                  ServerState::ROLE_COORDINATOR);
  return std::make_shared<ReplicatedStateCoordinatorMethods>(
      server, std::move(databaseName));
}
