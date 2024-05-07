////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
/// @author Ewout Prangsma
////////////////////////////////////////////////////////////////////////////////
#pragma once
#ifndef ARANGO_CXX_DRIVER_CONNECTION
#define ARANGO_CXX_DRIVER_CONNECTION

#include <memory>
#include <string>

#include "loop.h"
#include "message.h"
#include "types.h"

namespace arangodb { namespace fuerte { inline namespace v1 {
// Connection is the base class for a connection between a client
// and a server.
// Different protocols (HTTP) are implemented in derived classes.
// For details of the setup see the top of src/GeneralConnection.h .
class Connection : public std::enable_shared_from_this<Connection> {
  friend class ConnectionBuilder;

 public:
  virtual ~Connection();

  ///  Connection state
  ///  Created
  ///  +
  ///  |  +-------------------+--> Closed
  ///  |  |                   |
  ///  v  +                +
  ///  Connecting +-----> Connected
  enum class State : uint8_t {
    Created = 0,
    Connecting = 1,
    Connected = 2,
    Closed = 3  /// closed permanently
  };

  /// @brief Send a request to the server and wait into a response it received.
  std::unique_ptr<Response> sendRequest(std::unique_ptr<Request> r);

  /// @brief Send a request to the server and wait into a response it received.
  /// @param r request that is copied
  std::unique_ptr<Response> sendRequest(Request const& r) {
    auto copy = std::make_unique<Request>(r);
    return sendRequest(std::move(copy));
  }

  /// @brief Send a request to the server and return immediately.
  /// When a response is received or an error occurs, the corresponding
  /// callbackis called. The callback is executed on a specific
  /// IO-Thread for this connection.
  virtual void sendRequest(std::unique_ptr<Request> r, RequestCallback cb) = 0;

  /// @brief Send a request to the server and return immediately.
  /// When a response is received or an error occurs, the corresponding
  /// callbackis called. The callback is executed on a specific
  /// IO-Thread for this connection.
  void sendRequest(Request const& r, RequestCallback cb) {
    auto copy = std::make_unique<Request>(r);
    return sendRequest(std::move(copy), cb);
  }

  /// @brief Return the number of requests that have not yet finished.
  virtual std::size_t requestsLeft() const = 0;

  /// @brief connection state
  virtual State state() const = 0;

  /// @brief cancel the connection, unusable afterwards
  virtual void cancel() = 0;

  /// @brief endpoint we are connected to
  std::string endpoint() const;

 protected:
  Connection(detail::ConnectionConfiguration const& conf) : _config(conf) {}

  // Invoke the configured ConnectionFailureCallback (if any)
  void onFailure(Error errorCode, const std::string& errorMessage) {
    if (_config._onFailure) {
      try {
        _config._onFailure(errorCode, errorMessage);
      } catch (...) {
      }
    }
  }

  const detail::ConnectionConfiguration _config;
};

/** The connection Builder is a class that allows the easy configuration of
 *  connections. We decided to use the builder pattern because the connections
 *  have too many options to put them all in a single constructor. When you have
 *  passed all your options call connect() in order to receive a shared_ptr to a
 *  connection. Remember to use the "->" operator to access the connections
 *  members.
 */
class ConnectionBuilder {
 public:
  std::string host() const { return _conf._host; }
  std::string port() const { return _conf._port; }

  /// @brief takes url in the form (http)[s]://(ip|hostname):port
  /// also supports the syntax "http+tcp://", "http+unix://" etc
  ConnectionBuilder& endpoint(std::string const& spec);

  /// @brief get the normalized endpoint
  std::string normalizedEndpoint() const;

  // Create an connection and start opening it.
  std::shared_ptr<Connection> connect(EventLoopService& eventLoopService);

  /// @brief connect timeout (15s default)
  std::chrono::milliseconds connectTimeout() const {
    return _conf._connectTimeout;
  }
  /// @brief set the connect connection timeout (15s default)
  ConnectionBuilder& connectTimeout(std::chrono::milliseconds t) {
    _conf._connectTimeout = t;
    return *this;
  }

  /// @brief idle connection timeout (300s default)
  std::chrono::milliseconds idleTimeout() const {
    return _conf._idleTimeout;
  }
  /// @brief set the idle connection timeout (300s default)
  ConnectionBuilder& idleTimeout(std::chrono::milliseconds t) {
    _conf._idleTimeout = t;
    return *this;
  }

  /// @brief use an idle timeout
  ConnectionBuilder& useIdleTimeout(bool t) {
    _conf._useIdleTimeout = t;
    return *this;
  }

  // Set the authentication type of the connection
  AuthenticationType authenticationType() const {
    return _conf._authenticationType;
  }
  ConnectionBuilder& authenticationType(AuthenticationType t) {
    _conf._authenticationType = t;
    return *this;
  }
  // Set the username of the connection
  std::string user() const { return _conf._user; }
  ConnectionBuilder& user(std::string const& u) {
    _conf._user = u;
    return *this;
  }
  // Set the password of the connection
  std::string password() const { return _conf._password; }
  ConnectionBuilder& password(std::string const& p) {
    _conf._password = p;
    return *this;
  }

  // Set the jwt token of the connection
  std::string jwtToken() const { return _conf._jwtToken; }
  ConnectionBuilder& jwtToken(std::string const& t) {
    _conf._jwtToken = t;
    return *this;
  }

  /// @brief tcp, ssl or unix
  SocketType socketType() const { return _conf._socketType; }
  /// @brief protocol type
  ProtocolType protocolType() const { return _conf._protocolType; }
  void protocolType(ProtocolType pt) { _conf._protocolType = pt; }

  /// upgrade http1.1 to http2 connection (not necessary)
  ConnectionBuilder& upgradeHttp1ToHttp2(bool c) {
    _conf._upgradeH1ToH2 = c;
    return *this;
  }

  /// @brief should we verify the SSL host
  bool verifyHost() const { return _conf._verifyHost; }
  ConnectionBuilder& verifyHost(bool b) {
    _conf._verifyHost = b;
    return *this;
  }

  // Set a callback for connection failures that are not request specific.
  ConnectionBuilder& onFailure(ConnectionFailureCallback c) {
    _conf._onFailure = c;
    return *this;
  }

 private:
  detail::ConnectionConfiguration _conf;
};
}}}  // namespace arangodb::fuerte::v1
#endif
