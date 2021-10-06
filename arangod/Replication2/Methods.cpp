////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2021 ArangoDB GmbH, Cologne, Germany
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
#include <Futures/Future.h>

#include "Cluster/ServerState.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterFeature.h"
#include "Network/Methods.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedLog/LogLeader.h"
#include "Replication2/AgencyMethods.h"
#include "VocBase/vocbase.h"

#include "Methods.h"

using namespace arangodb;
using namespace arangodb::replication2;
using namespace arangodb::replication2::replicated_log;

struct ReplicatedLogMethodsDBServer final : ReplicatedLogMethods {
  explicit ReplicatedLogMethodsDBServer(TRI_vocbase_t& vocbase) : vocbase(vocbase) {}
  auto createReplicatedLog(replication2::agency::LogPlanSpecification const& spec) const
      -> futures::Future<Result> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto deleteReplicatedLog(LogId id) const -> futures::Future<Result> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto getReplicatedLogs() const
      -> futures::Future<std::unordered_map<arangodb::replication2::LogId, replicated_log::LogStatus>> override {
    return vocbase.getReplicatedLogs();
  }

  auto getLogStatus(LogId id) const
      -> futures::Future<replication2::replicated_log::LogStatus> override {
    return vocbase.getReplicatedLogById(id)->getParticipant()->getStatus();
  }

  auto getLogEntryByIndex(LogId id, LogIndex index) const
      -> futures::Future<std::optional<PersistingLogEntry>> override {
    return vocbase.getReplicatedLogLeaderById(id)->readReplicatedEntryByIndex(index);
  }

  auto slice(LogId id, LogIndex start, LogIndex stop) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    return vocbase.getReplicatedLogLeaderById(id)->copyInMemoryLog().getIteratorRange(start, stop);
  }

  auto poll(LogId id, LogIndex index, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    struct LimitingIterator : LogIterator {
      LimitingIterator(size_t limit, std::unique_ptr<LogIterator> source)
          : _limit(limit), _source(std::move(source)) {}
      auto next() -> std::optional<LogEntryView> override {
        if (_limit == 0) {
          return std::nullopt;
        }

        _limit -= 1;
        return _source->next();
      }

      std::size_t _limit;
      std::unique_ptr<LogIterator> _source;
    };

    return vocbase.getReplicatedLogLeaderById(id)->waitForIterator(index).thenValue(
        [limit](std::unique_ptr<LogIterator> iter) -> std::unique_ptr<LogIterator> {
          return std::make_unique<LimitingIterator>(limit, std::move(iter));
        });
  }

  auto tail(LogId id, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    auto log = vocbase.getReplicatedLogLeaderById(id)->copyInMemoryLog();
    auto stop = log.getNextIndex();
    auto start = stop.saturatedDecrement(limit);
    return log.getIteratorRange(start, stop);
  }

  auto head(LogId id, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    auto log = vocbase.getReplicatedLogLeaderById(id)->copyInMemoryLog();
    auto start = log.getFirstIndex();
    return log.getIteratorRange(start, start + limit);
  }

  auto insert(LogId id, LogPayload payload) const
      -> futures::Future<std::pair<LogIndex, replicated_log::WaitForResult>> override {
    auto log = vocbase.getReplicatedLogLeaderById(id);
    auto idx = log->insert(std::move(payload));
    return log->waitFor(idx).thenValue(
        [idx](auto&& result) { return std::make_pair(idx, std::forward<decltype(result)>(result)); });
  }

  auto insert(LogId id, TypedLogIterator<LogPayload>& iter) const
      -> futures::Future<std::pair<std::vector<LogIndex>, replicated_log::WaitForResult>> override {
    auto log = vocbase.getReplicatedLogLeaderById(id);
    auto indexes = std::vector<LogIndex>{};
    while (auto payload = iter.next()) {
      auto idx = log->insert(std::move(*payload));
      indexes.push_back(idx);
    }
    if (indexes.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, "multi insert list must not be empty");
    }

    return log->waitFor(indexes.back()).thenValue([indexes = std::move(indexes)](auto&& result) mutable {
      return std::make_pair(std::move(indexes), std::forward<decltype(result)>(result));
    });
  }

  auto release(LogId id, LogIndex index) const -> futures::Future<Result> override {
    auto log = vocbase.getReplicatedLogById(id);
    return log->getParticipant()->release(index);
  }
  TRI_vocbase_t& vocbase;
};

namespace {
struct VPackLogIterator final : LogIterator {
  explicit VPackLogIterator(std::shared_ptr<velocypack::Buffer<uint8_t>> buffer_ptr)
      : buffer(std::move(buffer_ptr)),
        iter(VPackSlice(buffer->data()).get("result")),
        end(iter.end()) {}

  auto next() -> std::optional<LogEntryView> override {
    while (iter != end) {
      return LogEntryView::fromVelocyPack(*iter++);
    }
    return std::nullopt;
  }

 private:
  std::shared_ptr<velocypack::Buffer<uint8_t>> buffer;
  VPackArrayIterator iter;
  VPackArrayIterator end;
};
}

struct ReplicatedLogMethodsCoordinator final : ReplicatedLogMethods {
  auto createReplicatedLog(replication2::agency::LogPlanSpecification const& spec) const
      -> futures::Future<Result> override {
    return replication2::agency::methods::createReplicatedLog(vocbase.name(), spec);
  }

  auto deleteReplicatedLog(LogId id) const -> futures::Future<Result> override {
    return replication2::agency::methods::deleteReplicatedLog(vocbase.name(), id);
  }

  auto getReplicatedLogs() const
      -> futures::Future<std::unordered_map<arangodb::replication2::LogId, replicated_log::LogStatus>> override {
    return vocbase.getReplicatedLogs();
  }

  auto getLogStatus(LogId id) const
      -> futures::Future<replication2::replicated_log::LogStatus> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id);
    network::RequestOptions opts;
    opts.database = vocbase.name();
    return network::sendRequest(pool, "server:" + getLogLeader(id), fuerte::RestVerb::Get, path)
        .thenValue([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          return replication2::replicated_log::LogStatus::fromVelocyPack(
              resp.slice().get("result"));
        });
  }

  auto getLogEntryByIndex(LogId id, LogIndex index) const
      -> futures::Future<std::optional<PersistingLogEntry>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "entry", index.value);
    network::RequestOptions opts;
    opts.database = vocbase.name();
    return network::sendRequest(pool, "server:" + getLogLeader(id), fuerte::RestVerb::Get,
                                path, {}, opts)
        .thenValue([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          auto entry =
              PersistingLogEntry::fromVelocyPack(resp.slice().get("result"));
          return std::optional<PersistingLogEntry>(std::move(entry));
        });
  }

  auto slice(LogId id, LogIndex start, LogIndex stop) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "slice");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["start"] = to_string(start);
    opts.parameters["stop"] = to_string(stop);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .thenValue([](network::Response&& resp) -> std::unique_ptr<LogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(resp.response().stealPayload());
        });
  }

  auto poll(LogId id, LogIndex index, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "poll");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["first"] = to_string(index);
    opts.parameters["limit"] = std::to_string(limit);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .thenValue([](network::Response&& resp) -> std::unique_ptr<LogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(resp.response().stealPayload());
        });
  }

  auto tail(LogId id, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "tail");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["limit"] = std::to_string(limit);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .thenValue([](network::Response&& resp) -> std::unique_ptr<LogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(resp.response().stealPayload());
        });
  }

  auto head(LogId id, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "head");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["limit"] = std::to_string(limit);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Get, path, {}, opts)
        .thenValue([](network::Response&& resp) -> std::unique_ptr<LogIterator> {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }

          return std::make_unique<VPackLogIterator>(resp.response().stealPayload());
        });
  }

  auto insert(LogId id, LogPayload payload) const
      -> futures::Future<std::pair<LogIndex, replicated_log::WaitForResult>> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "insert");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path, payload.dummy, opts)
        .thenValue([](network::Response&& resp) {
          if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
            THROW_ARANGO_EXCEPTION(resp.combinedResult());
          }
          auto result = resp.slice().get("result");
          auto waitResult = result.get("result");

          auto quorum = std::make_shared<replication2::replicated_log::QuorumData const>(
              waitResult.get("quorum"));
          auto commitIndex = waitResult.get("commitIndex").extract<LogIndex>();
          auto index = result.get("index").extract<LogIndex>();
          return std::make_pair(index, replicated_log::WaitForResult(commitIndex,
                                                                     std::move(quorum)));
        });
  }

  auto insert(LogId id, TypedLogIterator<LogPayload>& iter) const
      -> futures::Future<std::pair<std::vector<LogIndex>, replicated_log::WaitForResult>> override {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  auto release(LogId id, LogIndex index) const -> futures::Future<Result> override {
    auto path = basics::StringUtils::joinT("/", "_api/log", id, "release");

    network::RequestOptions opts;
    opts.database = vocbase.name();
    opts.parameters["index"] = to_string(index);
    return network::sendRequest(pool, "server:" + getLogLeader(id),
                                fuerte::RestVerb::Post, path, {}, opts)
        .thenValue([](network::Response&& resp) { return resp.combinedResult(); });
  }

  explicit ReplicatedLogMethodsCoordinator(TRI_vocbase_t& vocbase)
      : vocbase(vocbase),
        clusterInfo(vocbase.server().getFeature<ClusterFeature>().clusterInfo()),
        pool(vocbase.server().getFeature<NetworkFeature>().pool()) {}

 private:
  auto getLogLeader(LogId id) const -> ServerID {
    auto leader = clusterInfo.getReplicatedLogLeader(vocbase.name(), id);
    if (!leader) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED);
    }

    return *leader;
  }

  TRI_vocbase_t& vocbase;
  ClusterInfo& clusterInfo;
  network::ConnectionPool* pool;
};

auto ReplicatedLogMethods::createInstance(TRI_vocbase_t& vocbase)
    -> std::unique_ptr<ReplicatedLogMethods> {
  switch (ServerState::instance()->getRole()) {
    case ServerState::ROLE_COORDINATOR:
      return std::make_unique<ReplicatedLogMethodsCoordinator>(vocbase);
    case ServerState::ROLE_DBSERVER:
      return std::make_unique<ReplicatedLogMethodsDBServer>(vocbase);
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_NOT_IMPLEMENTED,
          "api only on available coordinators or dbservers");
  }
}

#if 0


namespace {

auto sendLogStatusRequest(network::ConnectionPool* pool, std::string const& server,
                          std::string const& database, LogId id)
    -> futures::Future<replication2::replicated_log::LogStatus> {
  auto path = basics::StringUtils::joinT("/", "_api/log", id);

  network::RequestOptions opts;
  opts.database = database;
  return network::sendRequest(pool, "server:" + server, fuerte::RestVerb::Get, path)
      .thenValue([](network::Response&& resp) {
        if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
          THROW_ARANGO_EXCEPTION(resp.combinedResult());
        }
        return replication2::replicated_log::LogStatus::fromVelocyPack(
            resp.slice().get("result"));
      });
}

auto sendReadEntryRequest(network::ConnectionPool* pool, std::string const& server,
                          std::string const& database, LogId id, LogIndex index)
    -> futures::Future<std::optional<PersistingLogEntry>> {
  auto path = basics::StringUtils::joinT("/", "_api/log", id, "readEntry", index.value);

  network::RequestOptions opts;
  opts.database = database;
  return network::sendRequest(pool, "server:" + server, fuerte::RestVerb::Get, path, {}, opts)
      .thenValue([](network::Response&& resp) {
        if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
          THROW_ARANGO_EXCEPTION(resp.combinedResult());
        }
        auto entry =
            PersistingLogEntry::fromVelocyPack(resp.slice().get("result"));
        return std::optional<PersistingLogEntry>(std::move(entry));
      });
}

auto sendTailRequest(network::ConnectionPool* pool, std::string const& server,
                     std::string const& database, LogId id, LogIndex first,
                     std::size_t limit) -> futures::Future<std::unique_ptr<LogIterator>> {
  auto path = basics::StringUtils::joinT("/", "_api/log", id, "tail");

  network::RequestOptions opts;
  opts.database = database;
  opts.parameters["first"] = to_string(first);
  opts.parameters["limit"] = std::to_string(limit);
  return network::sendRequest(pool, "server:" + server, fuerte::RestVerb::Get, path, {}, opts)
      .thenValue([](network::Response&& resp) -> std::unique_ptr<LogIterator> {
        if (resp.fail() || !fuerte::statusIsSuccess(resp.statusCode())) {
          THROW_ARANGO_EXCEPTION(resp.combinedResult());
        }

        struct VPackLogIterator final : LogIterator {
          explicit VPackLogIterator(std::shared_ptr<velocypack::Buffer<uint8_t>> buffer_ptr)
              : buffer(std::move(buffer_ptr)),
                iter(VPackSlice(buffer->data()).get("result")),
                end(iter.end()) {}

          auto next() -> std::optional<LogEntryView> override {
            while (iter != end) {
              return LogEntryView::fromVelocyPack(*iter++);
            }
            return std::nullopt;
          }

         private:
          std::shared_ptr<velocypack::Buffer<uint8_t>> buffer;
          VPackArrayIterator iter;
          VPackArrayIterator end;
        };

        return std::make_unique<VPackLogIterator>(resp.response().stealPayload());
      });
}
}  // namespace

struct ReplicatedLogMethodsCoord final : ReplicatedLogMethods {
  auto getLogLeader(LogId id) const {
    auto leader = clusterInfo.getReplicatedLogLeader(vocbase.name(), id);
    if (!leader) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_REPLICATION_REPLICATED_LOG_LEADER_RESIGNED);
    }

    return *leader;
  }

  auto insert(LogId id, LogPayload payload) const
      -> futures::Future<std::pair<LogIndex, replicated_log::WaitForResult>> override {
    return sendInsertRequest(pool, getLogLeader(id), vocbase.name(), id, std::move(payload));
  }

  auto getLogEntryByIndex(LogId id, LogIndex index) const
      -> futures::Future<std::optional<PersistingLogEntry>> override {
    return sendReadEntryRequest(pool, getLogLeader(id), vocbase.name(), id, index);
  }

  auto getLogStatus(LogId id) const
      -> futures::Future<replication2::replicated_log::LogStatus> override {
    return sendLogStatusRequest(pool, getLogLeader(id), vocbase.name(), id);
  }

  auto tailEntries(LogId id, LogIndex first, std::size_t limit) const
      -> futures::Future<std::unique_ptr<LogIterator>> override {
    return sendTailRequest(pool, getLogLeader(id), vocbase.name(), id, first, limit);
  }

  auto createReplicatedLog(replication2::agency::LogPlanSpecification const& spec) const
      -> futures::Future<Result> override {
    return replication2::agency::methods::createReplicatedLog(vocbase.name(), spec);
  }

  auto deleteReplicatedLog(LogId id) const -> futures::Future<Result> override {
    return replication2::agency::methods::deleteReplicatedLog(vocbase.name(), id);
  }

  auto updateTermSpecification(LogId id,
                               replication2::agency::LogPlanTermSpecification const& term) const
      -> futures::Future<Result> override {
    return replication2::agency::methods::updateTermSpecification(vocbase.name(), id, term);
  }

  explicit ReplicatedLogMethodsCoord(TRI_vocbase_t& vocbase, ClusterInfo& clusterInfo,
                                     network::ConnectionPool* pool)
      : vocbase(vocbase), clusterInfo(clusterInfo), pool(pool) {}

 private:
  TRI_vocbase_t& vocbase;
  ClusterInfo& clusterInfo;
  network::ConnectionPool* pool;
};

#endif
