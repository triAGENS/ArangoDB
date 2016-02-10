////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Aalekh Nigam
////////////////////////////////////////////////////////////////////////////////

#include "VelocyCommTask.h"

#include "Basics/MutexLocker.h"
#include "Basics/StringBuffer.h"
#include "Basics/logging.h"
#include "VelocyServer/VelocyHandler.h"
#include "VelocyServer/VelocyHandlerFactory.h"
#include "VelocyServer/GeneralServer.h"
#include "Scheduler/Scheduler.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief static initializers
////////////////////////////////////////////////////////////////////////////////

size_t const VelocyCommTask::MaximalHeaderSize = 1 * 1024 * 1024;       //   1 MB
size_t const VelocyCommTask::MaximalBodySize = 512 * 1024 * 1024;       // 512 MB
size_t const VelocyCommTask::MaximalPipelineSize = 1024 * 1024 * 1024;  //   1 GB

// typedef struct // @TODO: Remove this
// {
//   unsigned int x : 1; // minimum 1 bit
// } uint1_t;

// typedef struct 
// {
//   unsigned int y: 31; // minimum 31 bits
// } uint31_t;

// struct Vstream {
//   uint32_t length;
//   uint31_t chunk; //use .x to access value
//   uint1_t isFirstChunk; //use .y to access value
//   uint64_t messageId;
//   Builder s1;
//   Builder s2;
// };

////////////////////////////////////////////////////////////////////////////////
/// @brief constructs a new task
////////////////////////////////////////////////////////////////////////////////

VelocyCommTask::VelocyCommTask(GeneralServer* server, TRI_socket_t socket,
                           ConnectionInfo const& info, double keepAliveTimeout)
    : Task("VelocyCommTask"),
      SocketTask(socket, keepAliveTimeout),
      _connectionInfo(info),
      _server(server),
      _writeBuffers(),
      _writeBuffersStats(),
      _bodyLength(0),
      _isfirstChunk(),
      _requestPending(false),
      _closeRequested(false),
      _readRequestBody(false),
      _denyCredentials(false),
      _acceptDeflate(false),
      _newRequest(true),
      _isChunked(false),
      _request(nullptr),
      _vstreamVersion(GeneralRequest::VSTREAM_UNKNOWN),
      _requestType(GeneralRequest::VSTREAM_REQUEST_ILLEGAL),
      _fullUrl(),
      _origin(),
      _startPosition(0),
      _sinceCompactification(0),
      _originalBodyLength(0),
      _setupDone(false) {
  LOG_TRACE(
      "connection established, client %d, server ip %s, server port %d, client "
      "ip %s, client port %d",
      (int)TRI_get_fd_or_handle_of_socket(socket),
      _connectionInfo.serverAddress.c_str(), (int)_connectionInfo.serverPort,
      _connectionInfo.clientAddress.c_str(), (int)_connectionInfo.clientPort);

  // acquire a statistics entry and set the type to VelocyStream
  ConnectionStatisticsAgent::acquire();
  connectionStatisticsAgentSetStart();
  // connectionStatisticsAgentSetHttp(); @TODO: find STAT / TRI_stat_t structure and add velocystream support to it
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destructs a task
////////////////////////////////////////////////////////////////////////////////

VelocyCommTask::~VelocyCommTask() {
  LOG_TRACE("connection closed, client %d",
            (int)TRI_get_fd_or_handle_of_socket(_commSocket));

  // free write buffers and statistics
  for (auto& i : _writeBuffers) {
    delete i;
  }

  for (auto& i : _writeBuffersStats) {
    TRI_ReleaseRequestStatistics(i);
  }

  // free request
  delete _request;
}


////////////////////////////////////////////////////////////////////////////////
/// @brief handles response
////////////////////////////////////////////////////////////////////////////////

// @ TODO: remember to construct GeneralResponse that is Vpack specific 

void VelocyCommTask::handleResponse(GeneralResponse* response) {
  if (response->isChunked()) {
    _requestPending = true;
    _isChunked = true;
  } else {
    _requestPending = false;
    _isChunked = false;
  }

  // @Change from HTTP: This addResponse is for Vpack building
  addResponse(response);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads data from the socket
////////////////////////////////////////////////////////////////////////////////

bool VelocyCommTask::processRead() {
  if (_requestPending && _readBufferVstream == arangodb::velocypack::ValueType::Null) {
    return false;
  }
  bool handleRequest = false;

  // still trying to read the header fields
	  if (!_readRequestBody) {
	  	// starting a new request
	    if (_newRequest) {
	      // acquire a new statistics entry for the request
	      RequestStatisticsAgent::acquire();

	      _newRequest = false;
	      _vstreamVersion = GeneralRequest::VSTREAM_UNKNOWN;
	      _requestType = GeneralRequest::VSTREAM_REQUEST_ILLEGAL;
	      _fullUrl = "";
	      _denyCredentials = false;
	      _acceptDeflate = false;

	      _sinceCompactification++;
	    }

      requestStatisticsAgentSetReadStart();

	    if (_isFirstChunk && _readBufferVstream.byteSize() > MaximalHeaderSize) {
	      LOG_WARNING("maximal header size is %d, request header size is %d",
	                  (int)MaximalHeaderSize, (int)_readBufferVstream.byteSize());

	      // header is too large
	      GeneralResponse response(VstreamResponse::REQUEST_HEADER_FIELDS_TOO_LARGE,
	                            getCompatibility());

	      resetState(true);
	      handleResponse(&response);

	      return false;
	    }

	if (_readBufferVstream != arangodb::velocypack::ValueType::Null) {

	  // @TODO: Create a new handler in HandlerFactory for VelocyStream
    /// insert _request here
      // // check that we know, how to serve this request and update the connection
      // // information, i. e. client and server addresses and ports and create a
      // // request context for that request
      // _request = _server->handlerFactory()->createRequest(
      //     _connectionInfo, _readBuffer->c_str() + _startPosition,
      //     _readPosition - _startPosition);

      if (_request == nullptr) {
        LOG_ERROR("cannot generate request");

        // internal server error
        GeneralResponse response(VstreamResponse::SERVER_ERROR, getCompatibility());

        // we need to close the connection, because there is no way we
        // know how to remove the body and then continue
        resetState(true);
        handleResponse(&response);

        return false;
      }

      _request->setClientTaskId(_taskId);

      // check VSTREAM protocol version
      _vstreamVersion = _request->protocolVersion();

      // Currently we have aonly Vstream version 1.0 available
      if (_vstreamVersion != GeneralRequest::VSTREAM_1_0) {
        GeneralResponse response(GeneralResponse::VSTREAM_VERSION_NOT_SUPPORTED,
                              getCompatibility());

        // we need to close the connection, because there is no way we
        // know what to remove and then continue
        resetState(true);
        handleResponse(&response);

        return false;
      }

      // check max URL length
      _fullUrl = _request->fullUrl();

      if (_fullUrl.size() > 16384) {
        GeneralResponse response(VstreamResponse::REQUEST_URL_TOO_LONG,
                              getCompatibility());

        // we need to close the connection, because there is no way we
        // know what to remove and then continue
        resetState(true);
        handleResponse(&response);

        return false;
      }

      // update the connection information, i. e. client and server addresses
      // and ports
      _request->setProtocol(_server->protocol());

      LOG_TRACE("server port %d, client port %d",
                (int)_connectionInfo.serverPort,
                (int)_connectionInfo.clientPort);

      // keep track of the original value of the "origin" request header (if
      // any)
      // we need this value to handle CORS requests
      _origin = _request->header("origin");

      if (!_origin.empty()) {
        // check for Access-Control-Allow-Credentials header
        bool found;
        std::string const& allowCredentials =
            _request->header("access-control-allow-credentials", found);

        if (found) {
          _denyCredentials = !StringUtils::boolean(allowCredentials);
        }
      }

      // store the original request's type. we need it later when responding
      // (original request object gets deleted before responding)
      _requestType = _request->requestType();

      requestStatisticsAgentSetRequestType(_requestType);

      // handle different VSTREAM methods
      switch (_requestType) {
        case GeneralRequest::VSTREAM_REQUEST_GET:
        case GeneralRequest::VSTREAM_REQUEST_DELETE:
        case GeneralRequest::VSTREAM_REQUEST_HEAD:
        case GeneralRequest::VSTEAM_REQUEST_OPTIONS:
        case GeneralRequest::VSTREAM_REQUEST_POST:
        case GeneralRequest::VSTREAM_REQUEST_PUT:
        case GeneralRequest::VSTREAM_REQUEST_PATCH: 
        case GeneralRequest::VSTREAM_REQUEST_CRED:
        case GeneralRequest::VSTREAM_REQUEST_REGISTER:
        case GeneralRequest::VSTREAM_REQUEST_STATUS:{
          // technically, sending a body for an VSTREAM DELETE request is not
          // forbidden, but it is not explicitly supported
          bool const expectContentLength =
              (_requestType == GeneralRequest::VSTREAM_REQUEST_POST ||
               _requestType == GeneralRequest::VSTREAM_REQUEST_PUT ||
               _requestType == GeneralRequest::VSTREAM_REQUEST_PATCH ||
               _requestType == GeneralRequest::VSTREAM_REQUEST_OPTIONS ||
               _requestType == GeneralRequest::VSTREAM_REQUEST_DELETE ||
               _requestType == GeneralRequest::VSTREAM_REQUEST_CRED||
               _requestType == GeneralRequest::VSTREAM_REQUEST_REGISTER||
               _requestType == GeneralRequest::VSTREAM_REQUEST_STATUS);

        if(!_isFirstChunk){ 
          if (_readBufferVstream.byteSize() == 0) {
            handleRequest = true;
          }
        }  
          break;
        }

        default: {

          LOG_WARNING( "got corrupted VELOCYSTREAM request ");

          // bad request, method not allowed
          GeneralResponse response(VstreamResponse::METHOD_NOT_ALLOWED,
                                getCompatibility());

          // we need to close the connection, because there is no way we
          // know what to remove and then continue
          resetState(true);

          // force a socket close, response will be ignored!
          TRI_CLOSE_SOCKET(_commSocket);
          TRI_invalidatesocket(&_commSocket);

          // might delete this
          handleResponse(&response);

          return false;
        }
      }

      // .............................................................................
      // check if server is active
      // .............................................................................

      Scheduler const* scheduler =
          _server->scheduler(); 

      if (scheduler != nullptr && !scheduler->isActive()) {
        LOG_TRACE("cannot serve request - server is inactive");

        GeneralResponse response(GeneralResponse::VSTREAM_SERVICE_UNAVAILABLE,
                              getCompatibility());

        resetState(true);
        handleResponse(&response);

        return false;
      }

      // @TODO: handle write buffer here for vpack
      // if (_readRequestBody) {  
      //   bool found;
      //   std::string const& expect = _request->header("expect", found);

      //   if (found && StringUtils::trim(expect) == "100-continue") {
      //     LOG_TRACE("received a 100-continue request");

      //     auto buffer = std::make_unique<StringBuffer>(TRI_UNKNOWN_MEM_ZONE);
      //     buffer->appendText(
      //         TRI_CHAR_LENGTH_PAIR("HTTP/1.1 100 (Continue)\r\n\r\n"));

      //     _writeBuffers.push_back(buffer.get());
      //     buffer.release();

      //     _writeBuffersStats.push_back(nullptr);

      //     fillWriteBuffer();
      //   }
      // }

  	} 
  } 

  // readRequestBody might have changed, so cannot use else
  if (!_isFirstChunk) {

    // read "bodyLength" from read buffer and add this body to "GeneralRequest"
    _request->setBody(_readBufferVstream, _readBufferVstream.byteSize());

    LOG_TRACE("%s", std::to_string(_readBufferVstream.byteSize()));

    // remove body from read buffer and reset read position
    _readRequestBody = false;
    handleRequest = true;
  }

  if (!handleRequest) {
    return false;
  }
  if(!_isFirstChunk){
  	requestStatisticsAgentSetReadEnd();
  	requestStatisticsAgentAddReceivedBytes(_readBufferVstream.byteSize());
  }
  bool const isOptionsRequest =
      (_requestType == GeneralRequest::VSTREAM_REQUEST_OPTIONS);
  resetState(false);

  // .............................................................................
  // keep-alive handling
  // .............................................................................

  std::string connectionType =
      StringUtils::tolower(_request->header("connection"));

  if (connectionType == "close") {
    LOG_DEBUG("connection close requested by client");
    _closeRequested = true;
  } else if (connectionType != "keep-alive") {
    LOG_DEBUG("no keep-alive, connection close requested by client");
    _closeRequested = true;
  } else if (_keepAliveTimeout <= 0.0) {
    LOG_DEBUG("keep-alive disabled by admin");
    _closeRequested = true;
  }

  // .............................................................................
  // authenticate
  // .............................................................................

  auto const compatibility = _request->compatibility();

  GeneralResponse::GeneralResponseCode authResult =
      _server->handlerFactory()->authenticateRequest(_request); //@TODO: Create authentication handler for velocypack

  if (authResult == GeneralResponse::OK || isOptionsRequest) {
    if (isOptionsRequest) {
      processCorsOptions(compatibility);
    } else {
      processRequest(compatibility);
    }
  }

    // not found
  else if (authResult == VstreamResponse::NOT_FOUND) {
    GeneralResponse response(authResult, compatibility);

    Builder b;                                                                                                         
  	b.add(Value(ValueType::Object));                                                                                   
  	b.add("error", Value("true"));                                                                              
  	b.add("errorMessage", Value(TRI_errno_string(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND)));                                                                                  
  	b.add("code:", Value(to_string((int)authResult)));                                                 
  	b.add("errorNum", Value(to_string(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND)));
  	b.close()

    clearRequest();
    handleResponse(&b); // Create a response for builder objects
  }

  // forbidden
  else if (authResult == VstreamResponse::FORBIDDEN) {
    VstreamResponse response(authResult, compatibility);
    Builder b;                                                                                                         
  	b.add(Value(ValueType::Object));                                                                                   
  	b.add("error", Value("true"));                                                                              
  	b.add("errorMessage", Value("change password")));                                                                                  
  	b.add("code:", Value(to_string((int)authResult)));                                                 
  	b.add("errorNum", Value(to_string(TRI_ERROR_USER_CHANGE_PASSWORD)));
  	b.close()

    clearRequest();
    handleResponse(&b);
  } else{
  	GeneralResponse response(VstreamResponse::UNAUTHORIZED, compatibility);
    std::string const realm =
        "basic realm=\"" +
        _server->handlerFactory()->authenticationRealm(_request) + "\"";

    if (sendWwwAuthenticateHeader()) {
      response.setHeader(TRI_CHAR_LENGTH_PAIR("www-authenticate"),
                         realm.c_str());
    }

    clearRequest();
    handleResponse(&response);
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sends more chunked data
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::sendChunk(arangodb::velocypack::Builder buffer) {
  if (_isChunked) {
    TRI_ASSERT(buffer != nullptr);

    _writeBuffers.push_back(buffer);
    _writeBuffersStats.push_back(nullptr);

    fillWriteBuffer();
  } else {
    delete buffer;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief chunking is finished
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::finishedChunked() {
  auto buffer = std::make_unique<arangodb::velocypack::Builder>(TRI_UNKNOWN_MEM_ZONE, 6);

  _writeBuffers.push_back(buffer.get());
  buffer.release();
  _writeBuffersStats.push_back(nullptr);

  _isChunked = false;
  _requestPending = false;

  fillWriteBuffer();
  processRead();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief task set up complete
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::setupDone() {
  _setupDone.store(true, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads data from the socket
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::addResponse(VelocyResponse* response) {

  if (!_origin.empty()) {

    LOG_TRACE("handling CORS response");

    response->setHeader(TRI_CHAR_LENGTH_PAIR("access-control-expose-headers"),
                        "etag, content-encoding, content-length, location, "
                        "server, x-arango-errors, x-arango-async-id");

    // send back original value of "Origin" header
    response->setHeader(TRI_CHAR_LENGTH_PAIR("access-control-allow-origin"),
                        _origin);

    // send back "Access-Control-Allow-Credentials" header
    response->setHeader(
        TRI_CHAR_LENGTH_PAIR("access-control-allow-credentials"),
        (_denyCredentials ? "false" : "true"));
  }

  response->setHeader(TRI_CHAR_LENGTH_PAIR("connection"),
                      (_closeRequested ? "Close" : "Keep-Alive"));

  size_t const responseBodyLength = response->bodySize();

  if (_requestType == GeneralRequest::VSTREAM_REQUEST_HEAD) {

    response->headResponse(responseBodyLength);
  }

  // reserve a buffer with some spare capacity
  auto buffer = std::make_unique<arangodb::velocypack::Builder>(TRI_UNKNOWN_MEM_ZONE,
                                               responseBodyLength + 128);

  // write header
  response->writeHeader(buffer.get());

  // @TODO : Need to revaluate this, behavior is not same as Http should be VelocyPack instead
  // // write body
  // if (_requestType != GeneralRequest::VSTREAM_REQUEST_HEAD) {
  //   if (_isChunked) {
  //     if (0 != responseBodyLength) {
  //       buffer->appendHex(response->body().length());
  //       buffer->appendText(response->body());
  //     }
  //   } else {
  //     buffer->appendText(response->body());
  //   }
  // }

  _writeBuffers.push_back(buffer.get());
  auto b = buffer.release();

  LOG_TRACE("VSTREAM WRITE FOR %p: %s", (void*)this, b->c_str());

  // clear body
  response->body().clear();

  double const totalTime = RequestStatisticsAgent::elapsedSinceReadStart();

  _writeBuffersStats.push_back(RequestStatisticsAgent::transfer());

  // disable the following statement to prevent excessive logging of incoming
  // requests
  LOG_USAGE(
      ",\"velocystream-request\",\"%s\",\"%s\",\"%s\",%d,%llu,%llu,\"%s\",%.6f",
      _connectionInfo.clientAddress.c_str(),
      GeneralRequest::translateMethod(_requestType).c_str(),
      GeneralRequest::translateVersion(_vstreamVersion).c_str(),
      (int)response->responseCode(), (unsigned long long)_originalBodyLength,
      (unsigned long long)responseBodyLength, _fullUrl.c_str(), totalTime);

  // start output
  fillWriteBuffer();
}


////////////////////////////////////////////////////////////////////////////////
/// @brief fills the write buffer
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::fillWriteBuffer() {
  if (!hasWriteBuffer() && !_writeBuffers.empty()) {
    StringBuffer* buffer = _writeBuffers.front();
    _writeBuffers.pop_front();

    TRI_ASSERT(buffer != nullptr);

    TRI_request_statistics_t* statistics = _writeBuffersStats.front();
    _writeBuffersStats.pop_front();

    setWriteBuffer(buffer, statistics);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief handles CORS options
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::processCorsOptions(uint32_t compatibility) {
  std::string const allowedMethods = "DELETE, GET, HEAD, PATCH, POST, PUT, REGISTER";

  GeneralResponse response(GeneralResponse::VSTREAM_OK, compatibility);

  response.setHeader(TRI_CHAR_LENGTH_PAIR("allow"), allowedMethods);

  if (!_origin.empty()) {
    LOG_TRACE("got CORS preflight request");
    std::string const allowHeaders =
        StringUtils::trim(_request->header("access-control-request-headers"));

    // send back which VSTREAM methods are allowed for the resource
    // we'll allow all
    response.setHeader(TRI_CHAR_LENGTH_PAIR("access-control-allow-methods"),
                       allowedMethods);

    if (!allowHeaders.empty()) {
      response.setHeader(TRI_CHAR_LENGTH_PAIR("access-control-allow-headers"),
                         allowHeaders);
      LOG_TRACE("client requested validation of the following headers: %s",
                allowHeaders.c_str());
    }
    response.setHeader(TRI_CHAR_LENGTH_PAIR("access-control-max-age"), "1800");
  }

  clearRequest();
  handleResponse(&response);
}

///////////////////////////////////////////////////////////////////////////////
/// @brief processes a request
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::processRequest(uint32_t compatibility) {

  bool found;
  std::string const& acceptEncoding =
      _request->header("accept-encoding", found);

  if (found) {
    if (acceptEncoding.find("deflate") != std::string::npos) {
      _acceptDeflate = true;
    }
  }

  std::string const& asyncExecution = _request->header("x-arango-async", found);

  WorkItem::uptr<VstreamHandler> handler(
      _server->handlerFactory()->createHandler(_request));

  if (handler == nullptr) {
    LOG_TRACE("no handler is known, giving up");

    GeneralResponse response(GeneralResponse::VSTREAM_NOT_FOUND, compatibility);

    clearRequest();
    handleResponse(&response);

    return;
  }

  handler->setTaskId(_taskId, _loop);


  _request = nullptr;
  RequestStatisticsAgent::transfer(handler.get());


  bool ok = false;

  if (found && (asyncExecution == "true" || asyncExecution == "store")) {
    requestStatisticsAgentSetAsync();
    uint64_t jobId = 0;

    if (asyncExecution == "store") {

      ok = _server->handleRequestAsync(handler, &jobId);
    } else {

      ok = _server->handleRequestAsync(handler, nullptr);
    }

    if (ok) {
      GeneralResponse response(GeneralResponse::VSTREAM_ACCEPTED, compatibility);

      if (jobId > 0) {

        response.setHeader(TRI_CHAR_LENGTH_PAIR("x-arango-async-id"),
                           StringUtils::itoa(jobId));
      }

      handleResponse(&response);

      return;
    }
  }

  else {
    ok = _server->handleRequest(this, handler);
  }

  if (!ok) {
    GeneralResponse response(GeneralResponse::VSTREAM_SERVER_ERROR, compatibility);
    handleResponse(&response);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief clears the request object
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::clearRequest() {
  delete _request;
  _request = nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief resets the internal state
///
/// this method can be called to clean up when the request handling aborts
/// prematurely
////////////////////////////////////////////////////////////////////////////////

void VelocyCommTask::resetState(bool close) {
  size_t const COMPACT_EVERY = 500;

  if (close) {
    clearRequest();

    _requestPending = false;
    _closeRequested = true;

    _readPosition = 0;
    _bodyPosition = 0;
    _bodyLength = 0;
  } else {
    _requestPending = true;

    bool compact = false;

    if (_sinceCompactification > COMPACT_EVERY) {
      compact = true;
    } else if (_readBuffer->length() > MaximalPipelineSize) {
      compact = true;
    }

    if (compact) {
      _readBuffer->erase_front(_bodyPosition + _bodyLength);

      _sinceCompactification = 0;
      _readPosition = 0;
    } else {
      _readPosition = _bodyPosition + _bodyLength;
    }

    _bodyPosition = 0;
    _bodyLength = 0;
  }

  _newRequest = true;
  _readRequestBody = false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief decides whether or not we should send back a www-authenticate header
////////////////////////////////////////////////////////////////////////////////

bool VelocyCommTask::sendWwwAuthenticateHeader() const {
  bool found;
  _request->header("x-omit-www-authenticate", found);

  return !found;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get request compatibility
////////////////////////////////////////////////////////////////////////////////

int32_t VelocyCommTask::getCompatibility() const {
  if (_request != nullptr) {
    return _request->compatibility();
  }

  return GeneralRequest::MinCompatibility;
}


bool VelocyCommTask::setup(Scheduler* scheduler, EventLoop loop) {
  bool ok = SocketTask::setup(scheduler, loop);

  if (!ok) {
    return false;
  }

  _scheduler = scheduler;
  _loop = loop;
  
  setupDone();

  return true;
}

void VelocyCommTask::cleanup() { SocketTask::cleanup(); }

bool VelocyCommTask::handleEvent(EventToken token, EventType events) {
  bool result = SocketTask::handleEvent(token, events);

  if (_clientClosed) {
    _scheduler->destroyTask(this);
  }

  return result;
}


void VelocyCommTask::signalTask(TaskData* data) {
  // data response
  if (data->_type == TaskData::TASK_DATA_RESPONSE) {
    data->transfer(this);
    handleResponse(data->_response.get());
    processRead();
  }

  // data chunk
  else if (data->_type == TaskData::TASK_DATA_CHUNK) {
    size_t len = data->_data.size();

    if (0 == len) {
      finishedChunked();
    } else {
      arangodb::velocypack::Builder buffer;

      TRI_ASSERT(buffer != arangodb::velocypack::ValueType::Null);
      buffer.add(Value(ValueType::Object));
      buffer.add(Value(len));
      buffer.add(Value(data->_data.c_str()));
      buffer.close();
      sendChunk(buffer); // Create a sendChunk() overloaded function for velocypack::Builder Object.
    }
  }

  // do not know, what to do - give up
  else {
    _scheduler->destroyTask(this);
  }
}

bool VelocyCommTask::handleRead() {
  bool res = true;

  if (!_setupDone.load(std::memory_order_relaxed)) {
    return res;
  }

  if (!_closeRequested) {
    res = fillReadBuffer();

    while (processRead()) {
      if (_closeRequested) {
        break;
      }
    }
  } else {
    _clientClosed = true;
  }

  if (_clientClosed) {
    res = false;
    _server->handleCommunicationClosed(this);
  } else if (!res) {
    _clientClosed = true;
    _server->handleCommunicationFailure(this);
  }

  return res;
}


void VelocyCommTask::completedWriteBuffer() {
  _writeBuffer = nullptr;
  _writeLength = 0;

  if (_writeBufferStatistics != nullptr) {
    _writeBufferStatistics->_writeEnd = TRI_StatisticsTime();

    TRI_ReleaseRequestStatistics(_writeBufferStatistics);
    _writeBufferStatistics = nullptr;
  }

  fillWriteBuffer();

  if (!_clientClosed && _closeRequested && !hasWriteBuffer() &&
      _writeBuffers.empty() && !_isChunked) {
    _clientClosed = true;
    _server->handleCommunicationClosed(this);
  }
}

void VelocyCommTask::handleTimeout() {
  _clientClosed = true;
  _server->handleCommunicationClosed(this);
}
