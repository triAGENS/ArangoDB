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
/// @author Dr. Frank Celler
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#ifndef LIB_REST_HTTP_REQUEST_H
#define LIB_REST_HTTP_REQUEST_H 1

#include "Basics/Common.h"
#include "Basics/Dictionary.h"

#include "Basics/json.h"
#include "Basics/StringBuffer.h"
#include "Rest/ConnectionInfo.h"
#include "Rest/RequestContext.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>
#include <velocypack/vpack.h>
#include <velocypack/Options.h>
#include <velocypack/Parser.h>


namespace arangodb {
namespace rest {

////////////////////////////////////////////////////////////////////////////////
/// @brief http request
///
/// The http server reads the request string from the client and converts it
/// into an instance of this class. An http request object provides methods to
/// inspect the header and parameter fields.
////////////////////////////////////////////////////////////////////////////////

class GeneralRequest {
 private:
  GeneralRequest(GeneralRequest const&);
  GeneralRequest& operator=(GeneralRequest const&);

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief Protocol (http/velocystream) request type
  //////////////////////////////////////////////////////////////////////////////

  enum RequestType {
    HTTP_REQUEST_DELETE,
    HTTP_REQUEST_GET,
    HTTP_REQUEST_HEAD,
    HTTP_REQUEST_OPTIONS,
    HTTP_REQUEST_POST,
    HTTP_REQUEST_PUT,
    HTTP_REQUEST_PATCH,
    HTTP_REQUEST_ILLEGAL,
    VSTREAM_REQUEST_DELETE,
    VSTREAM_REQUEST_GET,
    VSTREAM_REQUEST_HEAD,
    VSTREAM_REQUEST_OPTIONS,
    VSTREAM_REQUEST_POST,
    VSTREAM_REQUEST_PUT,
    VSTREAM_REQUEST_PATCH,
    VSTREAM_REQUEST_ILLEGAL,
    VSTREAM_REQUEST_CRED, // This method is used for sending Authentication request,i.e; username and password.
    VSTREAM_REQUEST_REGISTER, // This Method is used for registering event of some kind
    VSTREAM_REQUEST_STATUS //Returns STATUS code and message for a given request
  };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief Protocol (Http/Vstream) version
  //////////////////////////////////////////////////////////////////////////////

  enum ProtocolVersion { HTTP_UNKNOWN, HTTP_1_0, HTTP_1_1, VSTREAM_UNKNOWN, VSTREAM_1_0 };

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief minimum API compatibility
  //////////////////////////////////////////////////////////////////////////////

  static int32_t const MinCompatibility;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief hard-coded minetype for batch requests
  //////////////////////////////////////////////////////////////////////////////

  static std::string const BatchContentType;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief hard-coded minetype for multipart/form-data
  //////////////////////////////////////////////////////////////////////////////

  static std::string const MultiPartContentType;

  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief translate the HTTP protocol version
  //////////////////////////////////////////////////////////////////////////////

  static std::string translateVersion(ProtocolVersion);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief translate an enum value into an HTTP method string
  //////////////////////////////////////////////////////////////////////////////

  static std::string translateMethod(RequestType);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief translate an HTTP method string into an enum value
  //////////////////////////////////////////////////////////////////////////////

  static RequestType translateMethod(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief append the request method string to a string buffer
  //////////////////////////////////////////////////////////////////////////////

  static void appendMethod(RequestType, arangodb::basics::StringBuffer*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the expected content-type for a subpart
  //////////////////////////////////////////////////////////////////////////////

  static std::string const& getPartContentType();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the expected content-type for a multipart message
  //////////////////////////////////////////////////////////////////////////////

  static std::string const& getMultipartContentType();

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief http request constructor
  ///
  /// Constructs a http request given the header string. A client request
  /// consists of two parts: the header and the body. For a GET request the
  /// body is always empty and all information about the request is delivered
  /// in the header. For a POST or PUT request some information is also
  /// delivered in the body. However, it is necessary to parse the header
  /// information, before the body can be read.
  //////////////////////////////////////////////////////////////////////////////

  GeneralRequest(ConnectionInfo const&, char const*, size_t, int32_t, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief velocystream(vstream) request constructor
  ///
  /// Constructs a velocystream request given the Velocypack(Vpack) builder object. A client 
  /// velocystream request consists of : length, chunk, isFirstChunk, messageId, n-Vpack objects. 
  //////////////////////////////////////////////////////////////////////////////

  GeneralRequest(ConnectionInfo const&, velocypack::Builder, uint32_t,
                        uint32_t , uint32_t, uint64_t , int32_t, bool);

  ~GeneralRequest();

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the protocol
  //////////////////////////////////////////////////////////////////////////////

  std::string const& protocol() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the connection info
  //////////////////////////////////////////////////////////////////////////////

  void setProtocol(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the connection info
  //////////////////////////////////////////////////////////////////////////////

  ConnectionInfo const& connectionInfo() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the connection info
  //////////////////////////////////////////////////////////////////////////////

  void setConnectionInfo(ConnectionInfo const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the http request type
  //////////////////////////////////////////////////////////////////////////////

  RequestType requestType() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the full url of the request
  //////////////////////////////////////////////////////////////////////////////

  std::string const& fullUrl() const { return _fullUrl; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the http request type
  //////////////////////////////////////////////////////////////////////////////

  void setRequestType(RequestType);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns whether HTTP protocol version is 1.0
  //////////////////////////////////////////////////////////////////////////////

  bool isHttp10() const { return _version == HTTP_1_0; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns whether HTTP protocol version is 1.1
  //////////////////////////////////////////////////////////////////////////////

  bool isHttp11() const { return _version == HTTP_1_1; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the  protocol (Http/Vstream) version
  //////////////////////////////////////////////////////////////////////////////

  ProtocolVersion protocolVersion() const { return _version; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the full request path
  ///
  /// The request path consists of the URL without the host and without any
  /// parameters.
  //////////////////////////////////////////////////////////////////////////////

  char const* requestPath() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief writes representation to string buffer
  //////////////////////////////////////////////////////////////////////////////

  void write(TRI_string_buffer_t*) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set the database name
  /// this is called when the request is processed by the ApplicationServer
  //////////////////////////////////////////////////////////////////////////////

  void setDatabaseName(std::string const& name) { _databaseName = name; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the database name
  //////////////////////////////////////////////////////////////////////////////

  std::string const& databaseName() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the authenticated user
  //////////////////////////////////////////////////////////////////////////////

  std::string const& user() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the authenticated user
  //////////////////////////////////////////////////////////////////////////////

  void setUser(std::string const&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the path of the request
  ///
  /// @note The @FA{path} must exists as long as the instance is alive and it
  ///       must be garbage collected by the caller.
  //////////////////////////////////////////////////////////////////////////////

  void setRequestPath(char const* path);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets the client task id
  //////////////////////////////////////////////////////////////////////////////

  uint64_t clientTaskId() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the client task id
  //////////////////////////////////////////////////////////////////////////////

  void setClientTaskId(uint64_t);

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the prefix path of the request
  ///
  /// The request path consists of the URL without the host and without any
  /// parameters.  The request path is split into two parts: the prefix, namely
  /// the part of the request path that was match by a handler and the suffix
  /// with
  /// all the remaining arguments.
  //////////////////////////////////////////////////////////////////////////////

  char const* prefix() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the prefix path of the request
  //////////////////////////////////////////////////////////////////////////////

  void setPrefix(char const* path);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns all suffix parts
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> const& suffix() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief adds a suffix part
  //////////////////////////////////////////////////////////////////////////////

  void addSuffix(std::string const& part);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set the request context
  //////////////////////////////////////////////////////////////////////////////

  void setRequestContext(RequestContext*, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief add request context
  //////////////////////////////////////////////////////////////////////////////

  RequestContext* getRequestContext() const { return _requestContext; }

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the content length
  //////////////////////////////////////////////////////////////////////////////

  int64_t contentLength() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns a header field
  ///
  /// Returns the value of a header field with given name. If no header field
  /// with the given name was specified by the client, the empty string is
  /// returned.
  ///
  /// @note The @FA{field} must be lowercase.
  //////////////////////////////////////////////////////////////////////////////

  char const* header(char const* key) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns a header field
  ///
  /// Returns the value of a header field with given name. If no header field
  /// with the given name was specified by the client, the empty string is
  /// returned. found is try if the client specified the header field.
  ///
  /// @note The @FA{field} must be lowercase.
  //////////////////////////////////////////////////////////////////////////////

  char const* header(char const* key, bool& found) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns all header fields
  ///
  /// Returns a copy of all header fields.
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, std::string> headers() const;

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the value of a key
  ///
  /// Returns the value of a key. The empty string is returned if key was not
  /// specified by the client.
  //////////////////////////////////////////////////////////////////////////////

  char const* value(char const* key) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the value of a key
  ///
  /// Returns the value of a key. The empty string is returned if key was not
  /// specified by the client. found is true if the client specified the key.
  //////////////////////////////////////////////////////////////////////////////

  char const* value(char const* key, bool& found) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns all values
  ///
  /// Returns all key/value pairs of the request.
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, std::string> values() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns all array values
  ///
  /// Returns all key/value pairs of the request.
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, std::vector<char const*>*> arrayValues() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns the value of a cookie
  ///
  /// Returns the value of a cookie.
  //////////////////////////////////////////////////////////////////////////////

  char const* cookieValue(char const* key) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief  returns the value of a cookie
  ///
  /// Returns the value of a cookie. found is true if the client specified the
  /// key.
  //////////////////////////////////////////////////////////////////////////////

  char const* cookieValue(char const* key, bool& found) const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief returns all cookies
  ///
  /// Returns all key/value pairs of the request.
  //////////////////////////////////////////////////////////////////////////////

  std::map<std::string, std::string> cookieValues() const;

  
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets the body
  //////////////////////////////////////////////////////////////////////////////

  char const* body() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets the body size
  //////////////////////////////////////////////////////////////////////////////

  size_t bodySize() const;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief register a copy of the body passed
  //////////////////////////////////////////////////////////////////////////////

  int setBody(char const* newBody, size_t length);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set a header field
  //////////////////////////////////////////////////////////////////////////////

  void setHeader(char const* key, size_t keyLength, char const* value);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief determine version compatibility
  //////////////////////////////////////////////////////////////////////////////

  int32_t compatibility();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets the request body as VelocyPackBuilder
  //////////////////////////////////////////////////////////////////////////////

  std::shared_ptr<VPackBuilder> toVelocyPack(VPackOptions const*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief gets the request body as TRI_json_t*
  //////////////////////////////////////////////////////////////////////////////

  TRI_json_t* toJson(char**);

  
 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief determine the header type
  //////////////////////////////////////////////////////////////////////////////

  static RequestType getRequestType(char const*, size_t const);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief parses the http header
  //////////////////////////////////////////////////////////////////////////////

  void parseHeader(char* ptr, size_t length);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief parses the velocystream header
  //////////////////////////////////////////////////////////////////////////////

  void parseHeader(velocypack::Builder, size_t);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get value from vpack and return as string.
  //////////////////////////////////////////////////////////////////////////////

  std::string getValue(velocypack::Slice);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the full url of the request
  //////////////////////////////////////////////////////////////////////////////

  void setFullUrl(char const* begin, char const* end);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the full url of the request (vstream)
  //////////////////////////////////////////////////////////////////////////////

  void setFullUrl(char const *str);
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets a key/value pair
  //////////////////////////////////////////////////////////////////////////////

  void setValue(char const* key, char const* value);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief sets the header values
  //////////////////////////////////////////////////////////////////////////////

  void setValues(char* buffer, char* end);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set array value
  //////////////////////////////////////////////////////////////////////////////

  void setArrayValue(char* key, size_t length, char const* value);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief set cookie
  //////////////////////////////////////////////////////////////////////////////

  void setCookie(char* key, size_t length, char const* value);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief parse value of a cookie header field
  //////////////////////////////////////////////////////////////////////////////

  void parseCookies(char const* buffer);

  
 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief complete request path, without protocol, host, and parameters
  //////////////////////////////////////////////////////////////////////////////

  char const* _requestPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief length of the complete vpack objects
  //////////////////////////////////////////////////////////////////////////////
  
  uint32_t _lengthVpack;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief length of total number of chunks in a Vpack enclosed message
  //////////////////////////////////////////////////////////////////////////////
    
  uint32_t chunk;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief to check if the given sequence is the first chunk
  ////////////////////////////////////////////////////////////////////////////// 

  uint32_t isFirstChunk;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief unique messageId; useful when concatenating the chunk
  ////////////////////////////////////////////////////////////////////////////// 

  uint64_t messageId;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief headers
  //////////////////////////////////////////////////////////////////////////////

  basics::Dictionary<char const*> _headers;

  // //////////////////////////////////////////////////////////////////////////////
  // /// @brief vpack header
  // //////////////////////////////////////////////////////////////////////////////
// TODO :: Remove this headersVpack;  
  // basics::Dictionary<Builder> _headersVpack;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief values
  //////////////////////////////////////////////////////////////////////////////

  basics::Dictionary<char const*> _values;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief array values
  //////////////////////////////////////////////////////////////////////////////

  basics::Dictionary<std::vector<char const*>*> _arrayValues;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief cookies
  //////////////////////////////////////////////////////////////////////////////

  basics::Dictionary<char const*> _cookies;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief content length
  //////////////////////////////////////////////////////////////////////////////

  int64_t _contentLength;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief body
  //////////////////////////////////////////////////////////////////////////////

  char* _body;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief body size
  //////////////////////////////////////////////////////////////////////////////

  size_t _bodySize;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief list of memory allocated which will be freed in the destructor
  //////////////////////////////////////////////////////////////////////////////

  std::vector<char*> _freeables;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief list of memory allocated (for velocypack) which will be freed in the 
  /// destructor
  //////////////////////////////////////////////////////////////////////////////

  std::vector<velocypack::Builder> _freeablesVpack;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the protocol used
  //////////////////////////////////////////////////////////////////////////////

  std::string _protocol;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief connection info for the server and the peer
  //////////////////////////////////////////////////////////////////////////////

  ConnectionInfo _connectionInfo;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the request type
  //////////////////////////////////////////////////////////////////////////////

  RequestType _type;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the prefix of the request path
  //////////////////////////////////////////////////////////////////////////////

  std::string _prefix;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the full url requested
  //////////////////////////////////////////////////////////////////////////////

  std::string _fullUrl;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the suffixes for the request path
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _suffix;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the HTTP version
  //////////////////////////////////////////////////////////////////////////////

  ProtocolVersion _version;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief database name
  //////////////////////////////////////////////////////////////////////////////

  std::string _databaseName;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief authenticated user
  //////////////////////////////////////////////////////////////////////////////

  std::string _user;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief request context
  //////////////////////////////////////////////////////////////////////////////

  RequestContext* _requestContext;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief default API compatibility
  /// the value is an ArangoDB version number in the following format:
  /// 10000 * major + 100 * minor (e.g. 10400 for ArangoDB 1.4)
  //////////////////////////////////////////////////////////////////////////////

  int32_t _defaultApiCompatibility;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not we are the owner of the context
  //////////////////////////////////////////////////////////////////////////////

  bool _isRequestContextOwner;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not overriding the HTTP method via custom headers
  /// (x-http-method, x-method-override or x-http-method-override) is allowed
  //////////////////////////////////////////////////////////////////////////////

  bool _allowMethodOverride;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief client identifier
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _clientTaskId;
};
}
}

#endif


