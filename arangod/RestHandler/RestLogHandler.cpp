////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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

#include "RestLogHandler.h"

#include <velocypack/Iterator.h>
#include <velocypack/Parser.h>
#include <velocypack/velocypack-aliases.h>

#include <Cluster/ServerState.h>
#include <Network/ConnectionPool.h>
#include <Network/Methods.h>
#include <Network/NetworkFeature.h>

#include <Agency/AsyncAgencyComm.h>
#include <Agency/TransactionBuilder.h>

#include "Replication2/ReplicatedLog/LogFollower.h"
#include "Replication2/ReplicatedLog/LogLeader.h"
#include "Replication2/ReplicatedLog/NetworkAttachedFollower.h"
#include "Replication2/ReplicatedLog/ReplicatedLog.h"
#include "Replication2/ReplicatedLog/types.h"
#include "Replication2/AgencyLogSpecification.h"

using namespace arangodb;
using namespace arangodb::replication2;

struct arangodb::ReplicatedLogMethods {
  virtual ~ReplicatedLogMethods() = default;
  virtual auto createReplicatedLog(replication2::agency::LogPlanSpecification const& spec) const
      -> futures::Future<Result> {
    return Result(TRI_ERROR_BAD_PARAMETER);
  }

  virtual auto deleteReplicatedLog(LogId id) const -> futures::Future<Result> {
    return Result(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto getReplicatedLogs() const
  -> futures::Future<std::unordered_map<arangodb::replication2::LogId,
      arangodb::replication2::replicated_log::LogStatus>> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto getLogStatus(LogId) const
      -> futures::Future<replication2::replicated_log::LogStatus> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto getLogEntryByIndex(LogId, LogIndex) const -> futures::Future<std::optional<LogEntry>> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto appendEntries(LogId, replicated_log::AppendEntriesRequest const&) const
      -> futures::Future<replicated_log::AppendEntriesResult> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto tailEntries(LogId, LogIndex) const
  -> futures::Future<std::unique_ptr<LogIterator>> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto insert(LogId, LogPayload) const
  -> futures::Future<std::pair<LogIndex, std::shared_ptr<replicated_log::QuorumData>>> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }

  virtual auto setTerm(LogId, replication2::agency::LogPlanTermSpecification const& term) const
      -> futures::Future<Result> {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
};

struct ReplicatedLogMethodsCoord final : ReplicatedLogMethods {
  auto createReplicatedLog(const replication2::agency::LogPlanSpecification& spec) const
      -> futures::Future<Result> override {
    AsyncAgencyComm ac;

    VPackBufferUInt8 trx;
    {
      std::string path = "arango/Plan/ReplicatedLogs/" + vocbase.name() + "/" +
                         std::to_string(spec.id.id());

      VPackBuilder builder(trx);
      arangodb::agency::envelope::into_builder(builder)
          .write()
          .emplace_object(path,
                          [&](VPackBuilder& builder) {
                            spec.toVelocyPack(builder);
                          })
          .inc("arango/Plan/Version")
          .precs()
          .isEmpty(path)
          .end()
          .done();
    }

    return ac.sendWriteTransaction(std::chrono::seconds(120), std::move(trx))
        .thenValue([](AsyncAgencyCommResult&& res) {
          return res.asResult();  // TODO
        });
  }

  auto setTerm(LogId id, replication2::agency::LogPlanTermSpecification const& term) const
  -> futures::Future<Result> override {
    AsyncAgencyComm ac;

    VPackBufferUInt8 trx;
    {
      std::string path = "arango/Plan/ReplicatedLogs/" + vocbase.name() + "/" +
                         std::to_string(id.id());
      std::string termPath = path + "/term";

      VPackBuilder builder(trx);
      arangodb::agency::envelope::into_builder(builder)
          .write()
          .emplace_object(termPath,
                          [&](VPackBuilder& builder) {
                            term.toVelocyPack(builder);
                          })
          .inc("arango/Plan/Version")
          .precs()
          .isNotEmpty(path)
          .end()
          .done();
    }

    return ac.sendWriteTransaction(std::chrono::seconds(120), std::move(trx))
        .thenValue([](AsyncAgencyCommResult&& res) {
          return res.asResult();  // TODO
        });
  }

  explicit ReplicatedLogMethodsCoord(TRI_vocbase_t& vocbase)
      : vocbase(vocbase) {}

 private:
  TRI_vocbase_t& vocbase;
};

struct ReplicatedLogMethodsDBServ final : ReplicatedLogMethods {
  auto createReplicatedLog(const replication2::agency::LogPlanSpecification& spec) const
      -> futures::Future<Result> override {
    return vocbase.createReplicatedLog(spec.id).result();
  }

  auto deleteReplicatedLog(LogId id) const -> futures::Future<Result> override {
    return vocbase.dropReplicatedLog(id);
  }

  auto getReplicatedLogs() const
  -> futures::Future<std::unordered_map<arangodb::replication2::LogId,
      arangodb::replication2::replicated_log::LogStatus>> override {
    return vocbase.getReplicatedLogs();
  }

  auto getLogStatus(LogId id) const
      -> futures::Future<replication2::replicated_log::LogStatus> override {
    return vocbase.getReplicatedLogById(id).getParticipant()->getStatus();
  }

  auto getLogEntryByIndex(LogId id, LogIndex idx) const -> futures::Future<std::optional<LogEntry>> override {
    return vocbase.getReplicatedLogLeaderById(id)->readReplicatedEntryByIndex(idx);
  }

  auto tailEntries(LogId id, LogIndex idx) const -> futures::Future<std::unique_ptr<LogIterator>> override {
    return vocbase.getReplicatedLogLeaderById(id)->waitForIterator(idx);
  }

  auto appendEntries(LogId id, replicated_log::AppendEntriesRequest const& req) const
      -> futures::Future<replicated_log::AppendEntriesResult> override {
    return vocbase.getReplicatedLogFollowerById(id)->appendEntries(req);
  }

  auto insert(LogId logId, LogPayload payload) const
      -> futures::Future<std::pair<LogIndex, std::shared_ptr<replicated_log::QuorumData>>> override {
    auto log = vocbase.getReplicatedLogLeaderById(logId);
    auto idx = log->insert(std::move(payload));
    auto f = log->waitFor(idx).thenValue(
        [idx](std::shared_ptr<replicated_log::QuorumData>&& quorum) {
          return std::make_pair(idx, quorum);
        });

    log->runAsyncStep();  // TODO
    return f;
  }

  explicit ReplicatedLogMethodsDBServ(TRI_vocbase_t& vocbase)
      : vocbase(vocbase) {}

 private:
  TRI_vocbase_t& vocbase;
};

RestStatus RestLogHandler::execute() {
  switch (ServerState::instance()->getRole()) {
    case ServerState::ROLE_DBSERVER:
      return executeByMethod(ReplicatedLogMethodsDBServ(_vocbase));
    case ServerState::ROLE_COORDINATOR:
      return executeByMethod(ReplicatedLogMethodsCoord(_vocbase));
    case ServerState::ROLE_SINGLE:
    case ServerState::ROLE_UNDEFINED:
    case ServerState::ROLE_AGENT:
      generateError(ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "api only on coordinators or dbservers");
      return RestStatus::DONE;
  }
}

RestStatus RestLogHandler::executeByMethod(ReplicatedLogMethods const& methods) {
  switch (_request->requestType()) {
    case rest::RequestType::GET:
      return handleGetRequest(methods);
    case rest::RequestType::POST:
      return handlePostRequest(methods);
    case rest::RequestType::DELETE_REQ:
      return handleDeleteRequest(methods);
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED, TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  }
  return RestStatus::DONE;
}


RestStatus RestLogHandler::handlePostRequest(ReplicatedLogMethods const& methods) {

  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {  // error message generated in parseVPackBody
    return RestStatus::DONE;
  }

  if (suffixes.empty()) {
    // create a new log
    replication2::agency::LogPlanSpecification spec(replication2::agency::from_velocypack, body);

    if (spec.currentTerm.has_value()) {
      spec.currentTerm.reset();
    }

    return waitForFuture(methods.createReplicatedLog(spec).thenValue([this](Result&& result) {
      if (result.ok()) {
        generateOk(rest::ResponseCode::OK, VPackSlice::emptyObjectSlice());
      } else {
        generateError(result);
      }
    }));
  }

  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect GET /_api/log/<log-id>");
    return RestStatus::DONE;
  }

  LogId logId{basics::StringUtils::uint64(suffixes[0])};

  if (auto& verb = suffixes[1]; verb == "insert") {
    return waitForFuture(
        methods.insert(logId, LogPayload{body.toJson()}).thenValue([this](auto&& result) {
          auto& [idx, quorum] = result;

          VPackBuilder response;
          {
            VPackObjectBuilder ob(&response);
            response.add("index", VPackValue(quorum->index.value));
            response.add("term", VPackValue(quorum->term.value));
            VPackArrayBuilder ab(&response, "quorum");
            for (auto& part : quorum->quorum) {
              response.add(VPackValue(part));
            }
          }
          LOG_DEVEL << "insert completed idx = " << idx.value;
          generateOk(rest::ResponseCode::ACCEPTED, response.slice());
        }));
  } else if(verb == "setTerm") {
    auto term = replication2::agency::LogPlanTermSpecification(replication2::agency::from_velocypack, body);
    return waitForFuture(methods.setTerm(logId, term).thenValue([this](auto&& result) {
      if (result.ok()) {
        generateOk(ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());
      } else {
        generateError(result);
      }
    }));
  } else if(verb == "becomeLeader") {
    if (!ServerState::instance()->isDBServer()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }
    auto& log = _vocbase.getReplicatedLogById(logId);

    auto term = LogTerm{body.get("term").getNumericValue<uint64_t>()};
    auto writeConcern = body.get("writeConcern").getNumericValue<std::size_t>();
    auto waitForSync = body.get("waitForSync").isTrue();

    std::vector<std::shared_ptr<replicated_log::AbstractFollower>> follower;
    for (auto const& part : VPackArrayIterator(body.get("follower"))) {
      auto partId = part.copyString();
      follower.emplace_back(std::make_shared<replicated_log::NetworkAttachedFollower>(server().getFeature<NetworkFeature>().pool(), partId, _vocbase.name(), logId));
    }

    replicated_log::LogLeader::TermData termData;
    termData.id = ServerState::instance()->getId();
    termData.term = term;
    termData.writeConcern = writeConcern;
    termData.waitForSync = waitForSync;
    log.becomeLeader(termData, follower);
    generateOk(rest::ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());
  } else if (verb == "becomeFollower") {
    if (!ServerState::instance()->isDBServer()) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
    }
    auto& log = _vocbase.getReplicatedLogById(logId);
    auto term = LogTerm{body.get("term").getNumericValue<uint64_t>()};
    auto leaderId = body.get("leader").copyString();
    log.becomeFollower(ServerState::instance()->getId(), term, leaderId);
    generateOk(rest::ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());

  } else if (verb == "appendEntries") {
    auto request = replicated_log::AppendEntriesRequest::fromVelocyPack(body);
    auto f = methods.appendEntries(logId, request).thenValue([this](replicated_log::AppendEntriesResult&& res) {
      VPackBuilder builder;
      res.toVelocyPack(builder);
      // TODO fix the result type here. Currently we always return the error under the
      //      `result` field. Maybe we want to change the HTTP status code as well?
      //      Don't forget to update the deserializer that reads the response!
      generateOk(rest::ResponseCode::ACCEPTED, builder.slice());
    });

    return waitForFuture(std::move(f));
  } else {
    generateError(
        rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
        "expecting one of the resources 'insert', ");

  }
  return RestStatus::DONE;
}

RestStatus RestLogHandler::handleGetRequest(ReplicatedLogMethods const& methods) {
  std::vector<std::string> const& suffixes = _request->decodedSuffixes();
  if (suffixes.empty()) {
    return waitForFuture(methods.getReplicatedLogs().thenValue([this](auto&& logs) {
      VPackBuilder builder;
      {
        VPackObjectBuilder ob(&builder);

        for (auto const& [idx, status] : logs) {
          builder.add(VPackValue(std::to_string(idx.id())));
          std::visit([&](auto const& status) { status.toVelocyPack(builder); }, status);
        }
      }

      generateOk(rest::ResponseCode::OK, builder.slice());
    }));
  }

  LogId logId{basics::StringUtils::uint64(suffixes[0])};

  if (suffixes.size() == 1) {
    return waitForFuture(methods.getLogStatus(logId).thenValue([this](auto&& status) {
      VPackBuilder buffer;
      std::visit([&](auto const& status) { status.toVelocyPack(buffer); }, status);
      generateOk(rest::ResponseCode::OK, buffer.slice());
    }));
  }

  auto const& verb = suffixes[1];

  if (verb == "tail") {
    if (suffixes.size() != 2) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expect GET /_api/log/<log-id>/tail");
      return RestStatus::DONE;
    }
    LogIndex logIdx{basics::StringUtils::uint64(_request->value("lastId"))};
    auto fut = methods.tailEntries(logId, logIdx).thenValue([&](std::unique_ptr<LogIterator> iter) {
      VPackBuilder builder;
      {
        VPackArrayBuilder ab(&builder);
        while (auto entry = iter->next()) {
          entry->toVelocyPack(builder);
        }
      }

      generateOk(rest::ResponseCode::OK, builder.slice());
    });

    return waitForFuture(std::move(fut));
  } else if (verb == "readEntry") {
    if (suffixes.size() != 3) {
      generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                    "expect GET /_api/log/<log-id>/readEntry/<id>");
      return RestStatus::DONE;
    }
    LogIndex logIdx{basics::StringUtils::uint64(suffixes[2])};

    return waitForFuture(
        methods.getLogEntryByIndex(logId, logIdx).thenValue([this](std::optional<LogEntry>&& entry) {
          if (entry) {
            VPackBuilder result;
            {
              VPackObjectBuilder builder(&result);
              result.add("index", VPackValue(entry->logIndex().value));
              result.add("term", VPackValue(entry->logTerm().value));

              {
                VPackParser parser;  // TODO remove parser and store vpack
                parser.parse(entry->logPayload().dummy);
                auto parserResult = parser.steal();
                result.add("payload", parserResult->slice());
              }
            }
            generateOk(rest::ResponseCode::OK, result.slice());

          } else {
            generateError(rest::ResponseCode::NOT_FOUND,
                          TRI_ERROR_HTTP_NOT_FOUND, "log index not found");
          }
        }));
  } else {
    generateError(rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
                  "expecting one of the resources 'dump', 'readEntry'");
  }
  return RestStatus::DONE;
}

RestStatus RestLogHandler::handleDeleteRequest(ReplicatedLogMethods const& methods) {

  std::vector<std::string> const& suffixes = _request->decodedSuffixes();

  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect DELETE /_api/log/<log-id>");
    return RestStatus::DONE;
  }

  LogId logId{basics::StringUtils::uint64(suffixes[0])};
  return waitForFuture(methods.deleteReplicatedLog(logId).thenValue([this](Result&& result) {
    if (!result.ok()) {
      generateError(result);
    } else {
      generateOk(rest::ResponseCode::ACCEPTED, VPackSlice::emptyObjectSlice());
    }
  }));
}

RestLogHandler::RestLogHandler(application_features::ApplicationServer& server,
                               GeneralRequest* req, GeneralResponse* resp)
    : RestVocbaseBaseHandler(server, req, resp) {}
RestLogHandler::~RestLogHandler() = default;
