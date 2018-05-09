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
/// @author Andreas Streichardt
////////////////////////////////////////////////////////////////////////////////

#include "Scheduler/SocketUnixDomain.h"

#include "Basics/StringBuffer.h"
#include <asio/write.hpp>

using namespace arangodb;

size_t SocketUnixDomain::writeSome(basics::StringBuffer* buffer, asio::error_code& ec) {
  return _socket.write_some(asio::buffer(buffer->begin(), buffer->length()), ec);
}
void SocketUnixDomain::asyncWrite(asio::mutable_buffers_1 const& buffer, AsyncHandler const& handler) {
  return asio::async_write(_socket, buffer, handler);
}
size_t SocketUnixDomain::readSome(asio::mutable_buffers_1 const& buffer, asio::error_code& ec) {
  return _socket.read_some(buffer, ec);
}
std::size_t SocketUnixDomain::available(asio::error_code& ec) {
  return _socket.available(ec);
}
void SocketUnixDomain::asyncRead(asio::mutable_buffers_1 const& buffer, AsyncHandler const& handler) {
  return _socket.async_read_some(buffer, handler);
}
void SocketUnixDomain::shutdownReceive(asio::error_code& ec) {
  _socket.shutdown(asio::local::stream_protocol::socket::shutdown_receive, ec);
}
void SocketUnixDomain::shutdownSend(asio::error_code& ec) {
  _socket.shutdown(asio::local::stream_protocol::socket::shutdown_send, ec);
}
void SocketUnixDomain::close(asio::error_code& ec) {
  if (_socket.is_open()) {
    _socket.close(ec); 
    if (ec && ec != asio::error::not_connected) {
      LOG_TOPIC(DEBUG, Logger::COMMUNICATION)
          << "closing socket failed with: " << ec.message();
    }
  }
}
