////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016-2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include <fuerte/connection.h>
#include <fuerte/FuerteLogger.h>

#include <boost/algorithm/string.hpp>

#include "HttpConnection.h"
#include "VstConnection.h"

namespace arangodb { namespace fuerte { inline namespace v1 {
// Create an connection and start opening it.
std::shared_ptr<Connection> ConnectionBuilder::connect(EventLoopService& loop) {
  std::shared_ptr<Connection> result;

  if (_conf._protocolType == ProtocolType::Vst) {
    FUERTE_LOG_DEBUG << "fuerte - creating velocystream connection\n";
    if (_conf._socketType == SocketType::Tcp) {
      result = std::make_shared<vst::VstConnection<SocketType::Tcp>>(loop, _conf);
    } else if (_conf._socketType == SocketType::Ssl) {
      result = std::make_shared<vst::VstConnection<SocketType::Ssl>>(loop, _conf);
    }
#ifdef ASIO_HAS_LOCAL_SOCKETS
     else if (_conf._socketType == SocketType::Unix) {
      result = std::make_shared<vst::VstConnection<SocketType::Unix>>(loop, _conf);
    }
#endif
  } else {
    // throw std::logic_error("http in vst test");
    FUERTE_LOG_DEBUG << "fuerte - creating http connection\n";
    if (_conf._socketType == SocketType::Tcp) {
      result = std::make_shared<http::HttpConnection<SocketType::Tcp>>(loop, _conf);
    } else if (_conf._socketType == SocketType::Ssl) {
      result = std::make_shared<http::HttpConnection<SocketType::Ssl>>(loop, _conf);
    }
#ifdef ASIO_HAS_LOCAL_SOCKETS
     else if (_conf._socketType == SocketType::Unix) {
      result = std::make_shared<http::HttpConnection<SocketType::Unix>>(loop, _conf);
    }
#endif
  }
  if (!result) {
    throw std::logic_error("unsupported socket or protocol type");
  }
  
  // Start the connection implementation
  result->startConnection();

  return result;
}
  
void parseSchema(std::string const& schema,
                 detail::ConnectionConfiguration& conf) {
  // non exthausive list of supported url schemas
  // "http+tcp://", "http+ssl://", "tcp://", "ssl://", "unix://", "http+unix://"
  // "vsts://", "vst://", "http://", "https://", "vst+unix://", "vst+tcp://"
  std::string proto = schema;
  std::string::size_type pos = schema.find('+');
  if (pos != std::string::npos && pos + 1 < schema.length()) {
    // got something like "http+tcp://"
    proto = schema.substr(0, pos);
    std::string socket = schema.substr(pos + 1);
    if (socket == "tcp" || socket == "srv") {
      conf._socketType = SocketType::Tcp;
    } else if (socket == "ssl") {
      conf._socketType = SocketType::Ssl;
    } else if (socket == "unix") {
      conf._socketType = SocketType::Unix;
    } else if (conf._socketType == SocketType::Undefined) {
      throw std::runtime_error(std::string("invalid socket type: ") + proto);
    }
    
    if (proto == "vst") {
      conf._protocolType = ProtocolType::Vst;
    } else if (proto == "http") {
      conf._protocolType = ProtocolType::Http;
    } else if (conf._protocolType == ProtocolType::Undefined) {
      throw std::runtime_error(std::string("invalid protocol: ") + proto);
    }
    
  } else { // got only protocol
    if (proto == "vst") {
      conf._socketType = SocketType::Tcp;
      conf._protocolType = ProtocolType::Vst;
    } else if (proto == "vsts") {
      conf._socketType = SocketType::Ssl;
      conf._protocolType = ProtocolType::Vst;
    } else if (proto == "http" || proto == "tcp") {
      conf._socketType = SocketType::Tcp;
      conf._protocolType = ProtocolType::Http;
    } else if (proto == "https" || proto == "ssl") {
      conf._socketType = SocketType::Ssl;
      conf._protocolType = ProtocolType::Http;
    } else if (proto == "unix") {
      conf._socketType = SocketType::Unix;
      conf._protocolType = ProtocolType::Http;
    } else if (conf._socketType == SocketType::Undefined ||
               conf._protocolType == ProtocolType::Undefined) {
      throw std::runtime_error(std::string("invalid schema: ") + proto);
    }
  }
}

#define IS_INVALID(c) (c == ' ' || c == '\r' || c == '\n')
#define LOWER(c)            (unsigned char)(c | 0x20)
#define IS_ALPHA(c)         (LOWER(c) >= 'a' && LOWER(c) <= 'z')
#define IS_NUM(c)           ((c) >= '0' && (c) <= '9')
#define IS_ALPHANUM(c)      (IS_ALPHA(c) || IS_NUM(c))
#define IS_HOST_CHAR(c)     (IS_ALPHANUM(c) || (c) == '.' || (c) == '-')
  
ConnectionBuilder& ConnectionBuilder::endpoint(std::string const& spec) {
  if (spec.empty()) {
    throw std::runtime_error("invalid empty endpoint spec");
  }
  
  // we need to handle unix:// urls seperately
  std::string::size_type pos = spec.find("://");
  if (std::string::npos != pos) {
    std::string schema = spec.substr(0, pos);
    boost::algorithm::to_lower(schema); // in-place
    parseSchema(schema, _conf);
  }
  
  if (_conf._socketType == SocketType::Unix) {
    if (std::string::npos != pos) {
      // unix:///a/b/c does not contain a port
      _conf._host = spec.substr(pos + 3);
    }
    return *this;
  }
  
  pos = (pos != std::string::npos) ? pos + 3 : 0;
  std::string::size_type x = pos + 1;
  if (spec[pos] == '[') { // ipv6 addresses contain colons
    pos++; // do not include '[' in actual address
    while (spec[x] != '\0' && spec[x] != ']') {
      if (IS_INVALID(spec[x])) { // we could validate this better
        throw std::runtime_error(std::string("invalid ipv6 address: ") + spec);
      }
      x++;
    }
    if (spec[x] != ']') {
      throw std::runtime_error(std::string("invalid ipv6 address: ") + spec);
    }
    _conf._host = spec.substr(pos, ++x - pos - 1); // do not include ']'
  } else {
    while (spec[x] != '\0' && spec[x] != '/' && spec[x] != ':') {
      if (!IS_HOST_CHAR(spec[x])) {
        throw std::runtime_error(std::string("invalid host in spec: ") + spec);
      }
      x++;
    }
    _conf._host = spec.substr(pos, x - pos);
  }
  if (_conf._host.empty()) {
    throw std::runtime_error(std::string("invalid host: ") + spec);
  }
  
  if (spec[x] == ':') {
    pos = ++x;
    while (spec[x] != '\0' && spec[x] != '/' && spec[x] != '?') {
      if (!IS_NUM(spec[x])) {
        throw std::runtime_error(std::string("invalid port in spec: ") + spec);
      }
      x++;
    }
    _conf._port = spec.substr(pos, x - pos);
    if (_conf._port.empty()) {
      throw std::runtime_error(std::string("invalid port in spec: ") + spec);
    }
  }
  
  return *this;
}
  
/// @brief get the normalized endpoint
std::string ConnectionBuilder::normalizedEndpoint() const {
  std::string endpoint;
  if (ProtocolType::Http == _conf._protocolType) {
    endpoint.append("http+");
  } else if (ProtocolType::Vst == _conf._protocolType) {
    endpoint.append("vst+");
  }
  
  if (SocketType::Tcp == _conf._socketType) {
    endpoint.append("tcp://");
  } else if (SocketType::Ssl == _conf._socketType) {
    endpoint.append("ssl://");
  } else if (SocketType::Unix == _conf._socketType) {
    endpoint.append("unix://");
  }
  
  endpoint.append(_conf._host);
  endpoint.push_back(':');
  endpoint.append(_conf._port);
  
  return endpoint;
}
  
}}}  // namespace arangodb::fuerte::v1
