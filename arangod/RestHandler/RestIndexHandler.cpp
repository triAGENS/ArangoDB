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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RestIndexHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "RestServer/VocbaseContext.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/Events.h"
#include "Utils/ExecContext.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Indexes.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestIndexHandler::RestIndexHandler(ArangodServer& server,
                                   GeneralRequest* request,
                                   GeneralResponse* response)
    : RestVocbaseBaseHandler(server, request, response) {}

RestIndexHandler::~RestIndexHandler() {
  std::unique_lock<std::mutex> locker(_mutex);
  shutdownBackgroundThread();
}

RestStatus RestIndexHandler::execute() {
  // extract the request type
  rest::RequestType const type = _request->requestType();
  if (type == rest::RequestType::GET) {
    if (_request->suffixes().size() == 1 &&
        _request->suffixes()[0] == "selectivity") {
      return getSelectivityEstimates();
    }
    return getIndexes();
  } else if (type == rest::RequestType::POST) {
    return createIndex();
  } else if (type == rest::RequestType::DELETE_REQ) {
    return dropIndex();
  } else if (type == rest::RequestType::PATCH) {
   return updateIndex();
  } else {
    generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                  TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
    return RestStatus::DONE;
  }
}

RestStatus RestIndexHandler::continueExecute() {
  TRI_ASSERT(_request->requestType() == rest::RequestType::POST);

  std::unique_lock<std::mutex> locker(_mutex);

  shutdownBackgroundThread();

  if (_createInBackgroundData.result.ok()) {
    TRI_ASSERT(_createInBackgroundData.response.slice().isObject());

    VPackSlice created =
        _createInBackgroundData.response.slice().get("isNewlyCreated");
    auto r = created.isBool() && created.getBool() ? rest::ResponseCode::CREATED
                                                   : rest::ResponseCode::OK;

    generateResult(r, _createInBackgroundData.response.slice());
  } else {
    if (_createInBackgroundData.result.is(TRI_ERROR_FORBIDDEN) ||
        _createInBackgroundData.result.is(TRI_ERROR_ARANGO_INDEX_NOT_FOUND)) {
      generateError(_createInBackgroundData.result);
    } else {  // http_server compatibility
      generateError(rest::ResponseCode::BAD,
                    _createInBackgroundData.result.errorNumber(),
                    _createInBackgroundData.result.errorMessage());
    }
  }

  return RestStatus::DONE;
}

void RestIndexHandler::shutdownExecute(bool isFinalized) noexcept {
  auto sg = arangodb::scopeGuard(
      [&]() noexcept { RestVocbaseBaseHandler::shutdownExecute(isFinalized); });
  if (isFinalized && _request->requestType() == rest::RequestType::POST) {
    std::unique_lock<std::mutex> locker(_mutex);
    shutdownBackgroundThread();
  }
}

void RestIndexHandler::shutdownBackgroundThread() {
  if (_createInBackgroundData.thread != nullptr) {
    _createInBackgroundData.thread->join();
    _createInBackgroundData.thread.reset();
  }
}

std::shared_ptr<LogicalCollection> RestIndexHandler::collection(
    std::string const& cName) {
  if (!cName.empty()) {
    if (ServerState::instance()->isCoordinator()) {
      return server()
          .getFeature<ClusterFeature>()
          .clusterInfo()
          .getCollectionNT(_vocbase.name(), cName);
    } else {
      return _vocbase.lookupCollection(cName);
    }
  }

  return nullptr;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief get index infos
// //////////////////////////////////////////////////////////////////////////////

RestStatus RestIndexHandler::getIndexes() {
  std::vector<std::string> const& suffixes = _request->suffixes();
  if (suffixes.empty()) {
    // .............................................................................
    // /_api/index?collection=<collection-name>
    // .............................................................................

    bool found = false;
    std::string cName = _request->value("collection", found);
    auto coll = collection(cName);
    if (coll == nullptr) {
      generateError(rest::ResponseCode::NOT_FOUND,
                    TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
      return RestStatus::DONE;
    }

    auto flags = Index::makeFlags(Index::Serialize::Estimates);
    if (_request->parsedValue("withStats", false)) {
      flags = Index::makeFlags(Index::Serialize::Estimates,
                               Index::Serialize::Figures);
    }
    bool withHidden = _request->parsedValue("withHidden", false);

    VPackBuilder indexes;
    Result res =
        methods::Indexes::getAll(coll.get(), flags, withHidden, indexes);
    if (!res.ok()) {
      generateError(rest::ResponseCode::BAD, res.errorNumber(),
                    res.errorMessage());
      return RestStatus::DONE;
    }

    TRI_ASSERT(indexes.slice().isArray());
    VPackBuilder tmp;
    tmp.openObject();
    tmp.add(StaticStrings::Error, VPackValue(false));
    tmp.add(StaticStrings::Code,
            VPackValue(static_cast<int>(ResponseCode::OK)));
    tmp.add("indexes", indexes.slice());
    tmp.add("identifiers", VPackValue(VPackValueType::Object));
    for (VPackSlice const& index : VPackArrayIterator(indexes.slice())) {
      VPackSlice id = index.get("id");
      VPackValueLength l = 0;
      char const* str = id.getString(l);
      tmp.add(str, l, index);
    }
    tmp.close();
    tmp.close();
    generateResult(rest::ResponseCode::OK, tmp.slice());
  } else if (suffixes.size() == 2) {
    // .............................................................................
    // /_api/index/<collection-name>/<index-identifier>
    // .............................................................................

    std::string const& cName = suffixes[0];
    auto coll = collection(cName);
    if (coll == nullptr) {
      generateError(rest::ResponseCode::NOT_FOUND,
                    TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
      return RestStatus::DONE;
    }

    std::string const& iid = suffixes[1];
    VPackBuilder tmp;
    tmp.add(VPackValue(cName + TRI_INDEX_HANDLE_SEPARATOR_CHR + iid));

    VPackBuilder output;
    Result res = methods::Indexes::getIndex(coll.get(), tmp.slice(), output);
    if (res.ok()) {
      VPackBuilder b;
      b.openObject();
      b.add(StaticStrings::Error, VPackValue(false));
      b.add(StaticStrings::Code,
            VPackValue(static_cast<int>(ResponseCode::OK)));
      b.close();
      output = VPackCollection::merge(output.slice(), b.slice(), false);
      generateResult(rest::ResponseCode::OK, output.slice());
    } else {
      generateError(res);
    }
  } else {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER);
  }
  return RestStatus::DONE;
}

RestStatus RestIndexHandler::getSelectivityEstimates() {
  // .............................................................................
  // /_api/index/selectivity?collection=<collection-name>
  // .............................................................................

  bool found = false;
  std::string const& cName = _request->value("collection", found);
  if (cName.empty()) {
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return RestStatus::DONE;
  }

  // transaction protects access onto selectivity estimates
  std::unique_ptr<transaction::Methods> trx;

  try {
    trx = createTransaction(cName, AccessMode::Type::READ, OperationOptions());
  } catch (basics::Exception const& ex) {
    if (ex.code() == TRI_ERROR_TRANSACTION_NOT_FOUND) {
      // this will happen if the tid of a managed transaction is passed in,
      // but the transaction hasn't yet started on the DB server. in
      // this case, we create an ad-hoc transaction on the underlying
      // collection
      trx = std::make_unique<SingleCollectionTransaction>(
          transaction::StandaloneContext::Create(_vocbase), cName,
          AccessMode::Type::READ);
    } else {
      throw;
    }
  }

  TRI_ASSERT(trx != nullptr);

  Result res = trx->begin();
  if (res.fail()) {
    generateError(res);
    return RestStatus::DONE;
  }

  LogicalCollection* coll = trx->documentCollection(cName);
  auto idxs = coll->getIndexes();

  VPackBuffer<uint8_t> buffer;
  VPackBuilder builder(buffer);
  builder.openObject();
  builder.add(StaticStrings::Error, VPackValue(false));
  builder.add(StaticStrings::Code,
              VPackValue(static_cast<int>(rest::ResponseCode::OK)));
  builder.add("indexes", VPackValue(VPackValueType::Object));
  for (std::shared_ptr<Index> idx : idxs) {
    if (idx->inProgress() || idx->isHidden()) {
      continue;
    }
    std::string name = coll->name();
    name.push_back(TRI_INDEX_HANDLE_SEPARATOR_CHR);
    name.append(std::to_string(idx->id().id()));
    if (idx->hasSelectivityEstimate() || idx->unique()) {
      builder.add(name, VPackValue(idx->selectivityEstimate()));
    }
  }
  builder.close();
  builder.close();

  generateResult(rest::ResponseCode::OK, std::move(buffer));
  return RestStatus::DONE;
}

RestStatus RestIndexHandler::updateIndex() {
  std::vector<std::string> const& suffixes = _request->suffixes();
  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }
  if (suffixes.size() != 2) {
    // FIXME: Do we need an event here?
    //events::UpdateIndex(_vocbase.name(), "(unknown)", "(unknown)",
    //                  TRI_ERROR_HTTP_BAD_PARAMETER);
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting PATCH /<collection-name>/<index-identifier>");
    return RestStatus::DONE;
  }
  std::string const& cName = suffixes[0];
  auto coll = collection(cName);
  if (coll == nullptr) {
    // FIXME: Do we need an event here?
    // events::UpdateIndex(_vocbase.name(), "(unknown)", "(unknown)",
    //                  TRI_ERROR_HTTP_BAD_PARAMETER);
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return RestStatus::DONE;
  }

  std::string const& iid = suffixes[1];
  VPackBuilder idBuilder;
  if (iid.starts_with(cName + TRI_INDEX_HANDLE_SEPARATOR_CHR)) {
    idBuilder.add(VPackValue(iid));
  } else {
    idBuilder.add(VPackValue(cName + TRI_INDEX_HANDLE_SEPARATOR_CHR + iid));
  }
  VPackBuilder result;
  Result res = methods::Indexes::update(coll.get(), idBuilder.slice(), body, result);
  if (res.ok()) {
    generateResult(rest::ResponseCode::OK, result.slice());
  } else {
    generateError(res);
  }
  return RestStatus::DONE;
}

RestStatus RestIndexHandler::createIndex() {
  std::vector<std::string> const& suffixes = _request->suffixes();
  bool parseSuccess = false;
  VPackSlice body = this->parseVPackBody(parseSuccess);
  if (!parseSuccess) {
    return RestStatus::DONE;
  }
  if (!suffixes.empty()) {
    events::CreateIndexEnd(_vocbase.name(), "(unknown)", body,
                           TRI_ERROR_BAD_PARAMETER);
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting POST " + _request->requestPath() +
                      "?collection=<collection-name>");
    return RestStatus::DONE;
  }

  bool found = false;
  std::string cName = _request->value("collection", found);
  if (!found) {
    events::CreateIndexEnd(_vocbase.name(), "(unknown)", body,
                           TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return RestStatus::DONE;
  }

  auto coll = collection(cName);
  if (coll == nullptr) {
    events::CreateIndexEnd(_vocbase.name(), cName, body,
                           TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return RestStatus::DONE;
  }

  VPackBuilder copy;
  if (body.get("collection").isNone()) {
    copy.openObject();
    copy.add("collection", VPackValue(cName));
    copy.close();
    copy = VPackCollection::merge(body, copy.slice(), false);
    body = copy.slice();
  }

  VPackBuilder indexInfo;
  indexInfo.add(body);

  auto execContext = std::unique_ptr<VocbaseContext>(
      VocbaseContext::create(*_request, _vocbase));
  // this is necessary, because the execContext will release the vocbase in its
  // dtor
  if (!_vocbase.use()) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  std::unique_lock<std::mutex> locker(_mutex);

  // the following callback is executed in a background thread
  auto cb = [this, self = shared_from_this(),
             execContext = std::move(execContext), collection = std::move(coll),
             body = std::move(indexInfo)] {
    ExecContextScope scope(execContext.get());
    {
      std::unique_lock<std::mutex> locker(_mutex);

      try {
        _createInBackgroundData.result =
            methods::Indexes::ensureIndex(collection.get(), body.slice(), true,
                                          _createInBackgroundData.response);

        if (_createInBackgroundData.result.ok()) {
          VPackSlice created =
              _createInBackgroundData.response.slice().get("isNewlyCreated");
          auto r = created.isBool() && created.getBool()
                       ? rest::ResponseCode::CREATED
                       : rest::ResponseCode::OK;

          VPackBuilder b;
          b.openObject();
          b.add(StaticStrings::Error, VPackValue(false));
          b.add(StaticStrings::Code, VPackValue(static_cast<int>(r)));
          b.close();
          _createInBackgroundData.response = VPackCollection::merge(
              _createInBackgroundData.response.slice(), b.slice(), false);
        }
      } catch (basics::Exception const& ex) {
        _createInBackgroundData.result = Result(ex.code(), ex.message());
      } catch (std::exception const& ex) {
        _createInBackgroundData.result = Result(TRI_ERROR_INTERNAL, ex.what());
      }
    }

    // notify REST handler
    SchedulerFeature::SCHEDULER->queue(RequestLane::INTERNAL_LOW,
                                       [self]() { self->wakeupHandler(); });
  };

  // start background thread
  _createInBackgroundData.thread = std::make_unique<std::thread>(std::move(cb));

  return RestStatus::WAITING;
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief was docuBlock JSF_get_api_database_delete
// //////////////////////////////////////////////////////////////////////////////
RestStatus RestIndexHandler::dropIndex() {
  std::vector<std::string> const& suffixes = _request->suffixes();
  if (suffixes.size() != 2) {
    events::DropIndex(_vocbase.name(), "(unknown)", "(unknown)",
                      TRI_ERROR_HTTP_BAD_PARAMETER);
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expecting DELETE /<collection-name>/<index-identifier>");
    return RestStatus::DONE;
  }

  std::string const& cName = suffixes[0];
  auto coll = collection(cName);
  if (coll == nullptr) {
    events::DropIndex(_vocbase.name(), cName, "(unknown)",
                      TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    generateError(rest::ResponseCode::NOT_FOUND,
                  TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    return RestStatus::DONE;
  }

  std::string const& iid = suffixes[1];
  VPackBuilder idBuilder;
  if (iid.starts_with(cName + TRI_INDEX_HANDLE_SEPARATOR_CHR)) {
    idBuilder.add(VPackValue(iid));
  } else {
    idBuilder.add(VPackValue(cName + TRI_INDEX_HANDLE_SEPARATOR_CHR + iid));
  }

  Result res = methods::Indexes::drop(coll.get(), idBuilder.slice());
  if (res.ok()) {
    VPackBuilder b;
    b.openObject();
    b.add("id", idBuilder.slice());
    b.add(StaticStrings::Error, VPackValue(false));
    b.add(StaticStrings::Code, VPackValue(static_cast<int>(ResponseCode::OK)));
    b.close();
    generateResult(rest::ResponseCode::OK, b.slice());
  } else {
    generateError(res);
  }
  return RestStatus::DONE;
}
