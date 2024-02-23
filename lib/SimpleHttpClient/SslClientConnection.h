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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stddef.h>
#include <cstdint>
#include <memory>

#include <openssl/ossl_typ.h>

#include "SimpleHttpClient/GeneralClientConnection.h"

namespace arangodb {
class Endpoint;
namespace basics {
class StringBuffer;
}
namespace httpclient {

////////////////////////////////////////////////////////////////////////////////
/// @brief client connection
////////////////////////////////////////////////////////////////////////////////

class SslClientConnection final : public GeneralClientConnection {
 private:
  SslClientConnection(SslClientConnection const&);
  SslClientConnection& operator=(SslClientConnection const&);
  bool cleanUpSocketFlags();
  bool setSocketToNonBlocking();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a new client connection
  //////////////////////////////////////////////////////////////////////////////

  SslClientConnection(application_features::CommunicationFeaturePhase& comm,
                      Endpoint* endpoint, double, double, size_t, uint64_t);

  SslClientConnection(application_features::CommunicationFeaturePhase& comm,
                      std::unique_ptr<Endpoint>& endpoint, double, double,
                      size_t, uint64_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroys a client connection
  //////////////////////////////////////////////////////////////////////////////

  ~SslClientConnection();

  uint64_t sslProtocol() const { return _sslProtocol; }

 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief internal initialization method, called from ctor
  //////////////////////////////////////////////////////////////////////////////

  void init(uint64_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief connect
  //////////////////////////////////////////////////////////////////////////////

  bool connectSocket() override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief disconnect
  //////////////////////////////////////////////////////////////////////////////

  void disconnectSocket() override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief write data to the connection
  //////////////////////////////////////////////////////////////////////////////

  bool writeClientConnection(void const*, size_t, size_t*) override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief read data from the connection
  //////////////////////////////////////////////////////////////////////////////

  bool readClientConnection(arangodb::basics::StringBuffer&,
                            bool& connectionClosed) override;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return whether the connection is readable
  //////////////////////////////////////////////////////////////////////////////

  bool readable() override;

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief the underlying session
  //////////////////////////////////////////////////////////////////////////////

  SSL* _ssl;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief SSL context
  //////////////////////////////////////////////////////////////////////////////

  SSL_CTX* _ctx;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief SSL version
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _sslProtocol;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief socket flags
  //////////////////////////////////////////////////////////////////////////////

  int _socketFlags;
};
}  // namespace httpclient
}  // namespace arangodb
