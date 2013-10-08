////////////////////////////////////////////////////////////////////////////////
/// @brief http request
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Copyright 2008-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "HttpRequest.h"

#include "BasicsC/conversions.h"
#include "BasicsC/tri-strings.h"
#include "BasicsC/utf8-helper.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Logger/Logger.h"

using namespace triagens::basics;
using namespace triagens::rest;

// -----------------------------------------------------------------------------
// --SECTION--                                                   local constants
// -----------------------------------------------------------------------------

static char const* EMPTY_STR = "";

// -----------------------------------------------------------------------------
// --SECTION--                                                 class HttpRequest
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief http request constructor
////////////////////////////////////////////////////////////////////////////////

HttpRequest::HttpRequest (ConnectionInfo const& info, 
                          char const* header, 
                          size_t length,
                          bool allowMethodOverride)
  : _requestPath(EMPTY_STR),
    _headers(5),
    _values(10),
    _arrayValues(10),
    _cookies(1),
    _contentLength(0),
    _body(0),
    _bodySize(0),
    _freeables(),
    _connectionInfo(info),
    _type(HTTP_REQUEST_ILLEGAL),
    _prefix(),
    _suffix(),
    _version(HTTP_UNKNOWN),
    _databaseName(),
    _user(),
    _requestContext(0),
    _isRequestContextOwner(false),
    _allowMethodOverride(allowMethodOverride) {

  // copy request - we will destroy/rearrange the content to compute the
  // headers and values in-place

  char* request = TRI_DuplicateString2Z(TRI_UNKNOWN_MEM_ZONE, header, length);

  if (request != 0) {
    _freeables.push_back(request);

    parseHeader(request, length);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief http request constructor
////////////////////////////////////////////////////////////////////////////////

HttpRequest::HttpRequest ()
  : _requestPath(EMPTY_STR),
    _headers(1),
    _values(1),
    _arrayValues(1),
    _cookies(1),
    _contentLength(0),
    _body(0),
    _bodySize(0),
    _freeables(),
    _connectionInfo(),
    _type(HTTP_REQUEST_ILLEGAL),
    _prefix(),
    _suffix(),
    _version(HTTP_UNKNOWN),
    _databaseName(),
    _user(),
    _requestContext(0),
    _isRequestContextOwner(false),
    _allowMethodOverride(false) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

HttpRequest::~HttpRequest () {
  basics::Dictionary< vector<char const*>* >::KeyValue const* begin;
  basics::Dictionary< vector<char const*>* >::KeyValue const* end;
  for (_arrayValues.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    vector<char const*>* v = begin->_value;
    delete v;
  }

  for (vector<char*>::iterator i = _freeables.begin();  i != _freeables.end();  ++i) {
    TRI_FreeString(TRI_UNKNOWN_MEM_ZONE, (*i));
  }
  
  if (_requestContext != 0 && _isRequestContextOwner) {
    // only delete if we are the owner of the context
    delete _requestContext;
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::requestPath () const {
  return _requestPath;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::write (TRI_string_buffer_t* buffer) const {
  const string& method = translateMethod(_type);

  TRI_AppendString2StringBuffer(buffer, method.c_str(), method.size());
  TRI_AppendCharStringBuffer(buffer, ' ');

  // do NOT url-encode the path, we need to distingush between
  // "/document/a/b" and "/document/a%2fb"

  TRI_AppendStringStringBuffer(buffer, _requestPath);

  // generate the request parameters
  basics::Dictionary<char const*>::KeyValue const* begin;
  basics::Dictionary<char const*>::KeyValue const* end;

  bool first = true;

  for (_values.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    if (first) {
      TRI_AppendCharStringBuffer(buffer, '?');
      first = false;
    }
    else {
      TRI_AppendCharStringBuffer(buffer, '&');
    }

    char const* value = begin->_value;

    TRI_AppendUrlEncodedStringStringBuffer(buffer, key);
    TRI_AppendCharStringBuffer(buffer, '=');
    TRI_AppendUrlEncodedStringStringBuffer(buffer, value);
  }

  TRI_AppendString2StringBuffer(buffer, " HTTP/1.1\r\n", 11);

  // generate the header fields
  for (_headers.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    const size_t keyLength = strlen(key);

    if (keyLength == 14 && memcmp(key, "content-length", keyLength) == 0) {
      continue;
    }

    TRI_AppendString2StringBuffer(buffer, key, keyLength);
    TRI_AppendString2StringBuffer(buffer, ": ", 2);

    char const* value = begin->_value;
    TRI_AppendStringStringBuffer(buffer, value);
    TRI_AppendString2StringBuffer(buffer, "\r\n", 2);
  }

  first = true;
  for (_cookies.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    if (first) {
      first = false;
      TRI_AppendString2StringBuffer(buffer, "Cookie: ", 8);
    }
    else {
      TRI_AppendString2StringBuffer(buffer, "; ", 2);
    }

    const size_t keyLength = strlen(key);
    TRI_AppendString2StringBuffer(buffer, key, keyLength);
    TRI_AppendString2StringBuffer(buffer, "=", 2);

    char const* value = begin->_value;
    TRI_AppendUrlEncodedStringStringBuffer(buffer, value);
  }

  TRI_AppendString2StringBuffer(buffer, "content-length: ", 16);
  TRI_AppendInt64StringBuffer(buffer, _contentLength);
  TRI_AppendString2StringBuffer(buffer, "\r\n\r\n", 4);

  if (_body != 0 && 0 < _bodySize) {
    TRI_AppendString2StringBuffer(buffer, _body, _bodySize);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

int64_t HttpRequest::contentLength () const {
  return _contentLength;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::header (char const* key) const {
  Dictionary<char const*>::KeyValue const* kv = _headers.lookup(key);

  if (kv == 0) {
    return EMPTY_STR;
  }
  else {
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::header (char const* key, bool& found) const {
  Dictionary<char const*>::KeyValue const* kv = _headers.lookup(key);

  if (kv == 0) {
    found = false;
    return EMPTY_STR;
  }
  else {
    found = true;
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

map<string, string> HttpRequest::headers () const {
  basics::Dictionary<char const*>::KeyValue const* begin;
  basics::Dictionary<char const*>::KeyValue const* end;

  map<string, string> result;

  for (_headers.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    result[key] = begin->_value;
  }

  result["content-length"] = StringUtils::itoa(_contentLength);

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::value (char const* key) const {
  Dictionary<char const*>::KeyValue const* kv = _values.lookup(key);

  if (kv == 0) {
    return EMPTY_STR;
  }
  else {
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::value (char const* key, bool& found) const {
  Dictionary<char const*>::KeyValue const* kv = _values.lookup(key);

  if (kv == 0) {
    found = false;
    return EMPTY_STR;
  }
  else {
    found = true;
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

map<string, string> HttpRequest::values () const {
  basics::Dictionary<char const*>::KeyValue const* begin;
  basics::Dictionary<char const*>::KeyValue const* end;

  map<string, string> result;

  for (_values.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    result[key] = begin->_value;
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

vector<char const*> const* HttpRequest::arrayValue (char const* key) const {
  Dictionary< vector<char const*>* >::KeyValue const* kv = _arrayValues.lookup(key);

  if (kv == 0) {
    return 0;
  }
  else {
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

vector<char const*> const* HttpRequest::arrayValue (char const* key, bool& found) const {
  Dictionary< vector<char const*>* >::KeyValue const* kv = _arrayValues.lookup(key);

  if (kv == 0) {
    found = false;
    return 0;
  }
  else {
    found = true;
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

map<string, vector<char const*>* > HttpRequest::arrayValues () const {
  basics::Dictionary< vector<char const*>* >::KeyValue const* begin;
  basics::Dictionary< vector<char const*>* >::KeyValue const* end;

  map<string, vector<char const*>* > result;

  for (_arrayValues.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    result[key] = begin->_value;
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::cookieValue (char const* key) const {
  Dictionary<char const*>::KeyValue const* kv = _cookies.lookup(key);

  if (kv == 0) {
    return EMPTY_STR;
  }
  else {
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::cookieValue (char const* key, bool& found) const {
  Dictionary<char const*>::KeyValue const* kv = _cookies.lookup(key);

  if (kv == 0) {
    found = false;
    return EMPTY_STR;
  }
  else {
    found = true;
    return kv->_value;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

map<string, string> HttpRequest::cookieValues () const {
  basics::Dictionary<char const*>::KeyValue const* begin;
  basics::Dictionary<char const*>::KeyValue const* end;

  map<string, string> result;

  for (_cookies.range(begin, end);  begin < end;  ++begin) {
    char const* key = begin->_key;

    if (key == 0) {
      continue;
    }

    result[key] = begin->_value;
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::body () const {
  return _body == 0 ? EMPTY_STR : _body;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

size_t HttpRequest::bodySize () const {
  return _bodySize;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

int HttpRequest::setBody (char const* newBody, 
                          size_t length) {
  _body = TRI_DuplicateString2Z(TRI_UNKNOWN_MEM_ZONE, newBody, length);

  if (_body == 0) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  _freeables.push_back(_body);

  _contentLength = (int64_t) length;
  _bodySize = length;

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief gets the request body as TRI_json_t*
////////////////////////////////////////////////////////////////////////////////

TRI_json_t* HttpRequest::toJson (char** errmsg) {
  TRI_json_t* json = TRI_Json2String(TRI_UNKNOWN_MEM_ZONE, body(), errmsg);

  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets a header field
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setHeader (char const* key, 
                             size_t keyLength, 
                             char const* value) {
  if (keyLength == 14 && memcmp(key, "content-length", keyLength) == 0) { // 14 = strlen("content-length")
    _contentLength = TRI_Int64String(value);
  }
  else if (keyLength == 6 && memcmp(key, "cookie", keyLength) == 0) { // 6 = strlen("cookie")
    parseCookies(value);
  }
  else {
    if (_allowMethodOverride && 
        keyLength >= 13 && 
        *key == 'x' && *(key + 1) == '-') {
      // handle x-... headers

      // override HTTP method?
      if ((keyLength == 13 && memcmp(key, "x-http-method", keyLength) == 0) ||
          (keyLength == 17 && memcmp(key, "x-method-override", keyLength) == 0) ||
          (keyLength == 22 && memcmp(key, "x-http-method-override", keyLength) == 0)) {
        string overridenType(value);
        StringUtils::tolowerInPlace(&overridenType);

        _type = getRequestType(overridenType.c_str(), overridenType.size());
      }
    }

    _headers.insert(key, keyLength, value);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the protocol
////////////////////////////////////////////////////////////////////////////////

const string& HttpRequest::protocol () const {
  return _protocol;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the connection info
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setProtocol (const string& protocol) {
  _protocol = protocol;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the connection info
////////////////////////////////////////////////////////////////////////////////

ConnectionInfo const& HttpRequest::connectionInfo () const {
  return _connectionInfo;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the connection info
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setConnectionInfo (ConnectionInfo const& info) {
  _connectionInfo = info;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the http request type
////////////////////////////////////////////////////////////////////////////////

HttpRequest::HttpRequestType HttpRequest::requestType () const {
  return _type;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the http request type
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setRequestType (HttpRequestType newType) {
  _type = newType;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the database name
////////////////////////////////////////////////////////////////////////////////

string const& HttpRequest::databaseName () const {
  return _databaseName;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the authenticated user
////////////////////////////////////////////////////////////////////////////////

string const& HttpRequest::user () const {
  return _user;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the authenticated user
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setUser (string const& user) {
  _user = user;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief determine the header type
////////////////////////////////////////////////////////////////////////////////

HttpRequest::HttpRequestType HttpRequest::getRequestType (const char* ptr, 
                                                          const size_t length) {
  switch (length) {
    case 3:
      if (ptr[0] == 'g' && ptr[1] == 'e' && ptr[2] == 't') {
        return HTTP_REQUEST_GET;
      }
      if (ptr[0] == 'p' && ptr[1] == 'u' && ptr[2] == 't') {
        return HTTP_REQUEST_PUT;
      }
      break;

    case 4:
      if (ptr[0] == 'p' && ptr[1] == 'o' && ptr[2] == 's' && ptr[3] == 't') {
        return HTTP_REQUEST_POST;
      }
      if (ptr[0] == 'h' && ptr[1] == 'e' && ptr[2] == 'a' && ptr[3] == 'd') {
        return HTTP_REQUEST_HEAD;
      }
      break;

    case 5:
      if (ptr[0] == 'p' && ptr[1] == 'a' && ptr[2] == 't' && ptr[3] == 'c' && ptr[4] == 'h') {
        return HTTP_REQUEST_PATCH;
      }
      break;

    case 6:
      if (ptr[0] == 'd' && ptr[1] == 'e' && ptr[2] == 'l' && ptr[3] == 'e' && ptr[4] == 't' && ptr[5] == 'e') {
        return HTTP_REQUEST_DELETE;
      }
      break;

    case 7:
      if (ptr[0] == 'o' && ptr[1] == 'p' && ptr[2] == 't' && ptr[3] == 'i' && ptr[4] == 'o' && ptr[5] == 'n' && ptr[6] == 's') {
        return HTTP_REQUEST_OPTIONS;
      }
      break;
  }

  return HTTP_REQUEST_ILLEGAL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parses the http header
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::parseHeader (char* ptr, size_t length) {
  char* start = ptr;
  char* end = start + length;
  const size_t versionLength = strlen("http/1.x");

  // current line number
  int lineNum = 0;

  // begin and end of current line
  char* lineBegin = 0;

  // split request
  while (start < end) {
    lineBegin = start;

    // .............................................................................
    // FIRST LINE
    // .............................................................................

    // check for request type (GET/POST in line 0), path and parameters
    if (lineNum == 0) {

      // split line at space
      char* e = lineBegin;

      for (;  e < end && *e != ' ' && *e != '\n';  ++e) {
        *e = ::tolower(*e);
      }

      // store key and value
      char* keyBegin = lineBegin;
      char* keyEnd = e;

      char* valueBegin = 0;
      char* valueEnd = 0;

      // we found a space (*end is '\0')
      if (*e == ' ') {

        // extract the value
        keyEnd = e;

        // trim value from the start
        while (e < end && *e == ' ') {
          ++e;
        }

        *keyEnd = '\0';

        // there is no value at all
        if (e == end) {
          valueBegin = valueEnd = keyEnd;
          start = end;
        }

        // there is only a NL
        else if (*e == '\n') {
          valueBegin = valueEnd = keyEnd;
          start = e + 1;
        }

        // find space
        else {
          valueBegin = e;

          while (e < end && *e != '\n' && *e != ' ') {
            ++e;
          }

          if (e == end) {
            valueEnd = e;
            start = end;
          }
          else {
            if (*e == '\n') {
              valueEnd = e;
              start = e + 1;

              // skip \r
              if (valueBegin < valueEnd && valueEnd[-1] == '\r') {
                --valueEnd;
              }
            }
            else {
              valueEnd = e;

              // HTTP protocol version is expected next
              // trim value
              while (e < end && *e == ' ') {
                ++e;
              }

              if ((size_t)(end - e) > versionLength) {
                if ((e[0] == 'h' || e[0] == 'H') &&
                    (e[1] == 't' || e[1] == 'T') &&
                    (e[2] == 't' || e[2] == 'T') &&
                    (e[3] == 'p' || e[3] == 'P') &&
                     e[4] == '/' &&
                     e[5] == '1' &&
                     e[6] == '.') {
                  if (e[7] == '1') {
                    _version = HTTP_1_1;
                  }
                  else if (e[7] == '0') {
                    _version = HTTP_1_0;
                  }
                  else {
                    _version = HTTP_UNKNOWN;
                  }

                  e += versionLength;
                }
              }

              // go on until eol
              while (e < end && *e != '\n') {
                ++e;
              }

              if (e == end) {
                start = end;
              }
              else {
                start = e + 1;
              }
            }

            *valueEnd = '\0';
          }
        }

        // check the key
        _type = getRequestType(keyBegin, keyEnd - keyBegin);

        // extract the path and decode the url and parameters
        if (_type != HTTP_REQUEST_ILLEGAL) {
          char* pathBegin = valueBegin;
          char* pathEnd = 0;

          char* paramBegin = 0;
          char* paramEnd = 0;

          // find a question mark or space
          char* f = pathBegin;

          // do NOT url-decode the path, we need to distingush between
          // "/document/a/b" and "/document/a%2fb"

          while (f < valueEnd && *f != '?' && *f != ' ' && *f != '\n') {
            ++f;
          }

          pathEnd = f;

          // look for database name in URL
          if (pathEnd - pathBegin >= 5) {
            char* q = pathBegin;

            if (q[0] == '/' && q[1] == '_' && q[2] == 'd' && q[3] == 'b' && q[4] == '/') {
              // request contains database name
              q += 5;
              pathBegin = q;

              // read until end of database name
              while (*q != '\0') {
                if (*q == '/' || *q == '?' || *q == ' ' || *q == '\n' || *q == '\r') {
                  break;
                }
                ++q;
              }
              
              _databaseName = string(pathBegin, q - pathBegin);

              pathBegin = q;
            }
          }

          // no space, question mark or end-of-line
          if (f == valueEnd) {
            paramEnd = paramBegin = pathEnd;

            // set full url = complete path
            setFullUrl(pathBegin, pathEnd);
          }

          // no question mark
          else if (*f == ' ' || *f == '\n') {
            *pathEnd = '\0';

            paramEnd = paramBegin = pathEnd;

            // set full url = complete path
            setFullUrl(pathBegin, pathEnd);
          }

          // found a question mark
          else {
            paramBegin = f + 1;
            paramEnd = paramBegin;

            while (paramEnd < valueEnd && *paramEnd != ' ' && *paramEnd != '\n') {
              ++paramEnd;
            }

            // set full url = complete path + url parameters
            setFullUrl(pathBegin, paramEnd);

            // now that the full url was saved, we can insert the null bytes
            *pathEnd = '\0';
            *paramEnd = '\0';
          }

          if (pathBegin < pathEnd) {
            setRequestPath(pathBegin);
          }

          if (paramBegin < paramEnd) {
            setValues(paramBegin, paramEnd);
          }
        }
      }

      // no space, either eol or newline
      else {
        if (e < end) {
          *keyEnd = '\0';
          start = e + 1;
        }
        else {
          start = end;
        }

        // check the key
        _type = getRequestType(keyBegin, keyEnd - keyBegin);
      }
    }

    // .............................................................................
    // OTHER LINES
    // .............................................................................

    else {

      // split line at colon
      char* e = lineBegin;

      for (;  e < end && *e != ':' && *e != '\n';  ++e) {
        *e = ::tolower(*e);
      }

      // store key and value
      char* keyBegin = lineBegin;
      char* keyEnd = e;

      // we found a colon (*end is '\0')
      if (*e == ':') {
        char* valueBegin = 0;
        char* valueEnd = 0;

        // extract the value
        keyEnd = e++;

        while (e < end && *e == ' ') {
          ++e;
        }

        while (keyBegin < keyEnd && keyEnd[-1] == ' ') {
          --keyEnd;
        }

        *keyEnd = '\0';

        // there is no value at all
        if (e == end) {
          valueBegin = valueEnd = keyEnd;
          start = end;
        }
        else if (*e == '\n') {
          valueBegin = valueEnd = keyEnd;
          start = e + 1;
        }

        // find \n
        else {
          valueBegin = e;

          while (e < end && *e != '\n') {
            ++e;
          }

          if (e == end) {
            valueEnd = e;
            start = end;
          }
          else {
            valueEnd = e;
            start = e + 1;
          }

          // skip \r
          if (valueBegin < valueEnd && valueEnd[-1] == '\r') {
            --valueEnd;
          }

          // skip trailing spaces
          while (valueBegin < valueEnd && valueEnd[-1] == ' ') {
            --valueEnd;
          }

          *valueEnd = '\0';
        }

        if (keyBegin < keyEnd) {
          setHeader(keyBegin, keyEnd - keyBegin, valueBegin);
        }
      }

      // we found no colon, either eol or newline. Take the whole line as key
      else {
        if (e < end) {
          *keyEnd = '\0';
          start = e + 1;
        }
        else {
          start = end;
        }

        // skip \r
        if (keyBegin < keyEnd && keyEnd[-1] == '\r') {
          --keyEnd;
        }

        // use empty value
        if (keyBegin < keyEnd) {
          setHeader(keyBegin, keyEnd - keyBegin, keyEnd);
        }
      }
    }

    lineNum++;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the full url
/// this will create a copy of the characters in the range, so the original
/// range can be modified afterwards
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setFullUrl (char const* begin, char const* end) {
  assert(begin != 0);
  assert(end != 0);
  assert(begin <= end);

  _fullUrl = string(begin, end - begin);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the path of the request
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setRequestPath (char const* path) {
  _requestPath = path;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the header values
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setValues (char* buffer, char* end) {
  char* keyBegin = 0;
  char* key = 0;

  char* valueBegin = 0;
  char* value = 0;

  enum { KEY, VALUE } phase = KEY;
  enum { NORMAL, HEX1, HEX2 } reader = NORMAL;

  int hex = 0;

  char const AMB     = '&';
  char const EQUAL   = '=';
  char const PERCENT = '%';
  char const PLUS    = '+';

  for (keyBegin = key = buffer; buffer < end; buffer++) {
    char next = *buffer;

    if (phase == KEY && next == EQUAL) {
      phase = VALUE;

      valueBegin = value = buffer + 1;

      continue;
    }
    else if (next == AMB) {
      phase = KEY;

      *key = '\0';

      // check for missing value phase
      if (valueBegin == 0) {
        valueBegin = value = key;
      }
      else {
        *value = '\0';
      }

      if (key - keyBegin > 2 && (*(key - 2)) == '[' &&  (*(key - 1)) == ']') {
        // found parameter xxx[]
        *(key - 2) = '\0';
        setArrayValue(keyBegin, key - keyBegin - 2, valueBegin);
      }
      else {
        _values.insert(keyBegin, key - keyBegin, valueBegin);
      }

      keyBegin = key = buffer + 1;
      valueBegin = value = 0;

      continue;
    }
    else if (next == PERCENT) {
      reader = HEX1;
      continue;
    }
    else if (reader == HEX1) {
      int h1 = StringUtils::hex2int(next, -1);

      if (h1 == -1) {
        reader = NORMAL;
        --buffer;
        continue;
      }

      hex = h1 * 16;
      reader = HEX2;
      continue;
    }
    else if (reader == HEX2) {
      int h1 = StringUtils::hex2int(next, -1);

      if (h1 == -1) {
        --buffer;
      }
      else {
        hex += h1;
      }

      reader = NORMAL;
      next = static_cast<char>(hex);
    }
    else if (next == PLUS) {
      next = ' ';
    }

    if (phase == KEY) {
      *key++ = next;
    }
    else {
      *value++ = next;
    }
  }

  if (keyBegin != key) {
    *key = '\0';

    // check for missing value phase
    if (valueBegin == 0) {
      valueBegin = value = key;
    }
    else {
      *value = '\0';
    }

    if (key - keyBegin > 2 && (*(key - 2)) == '[' &&  (*(key - 1)) == ']') {
      // found parameter xxx[]
      *(key - 2) = '\0';
      setArrayValue(keyBegin, key - keyBegin - 2, valueBegin);
    }
    else {
      _values.insert(keyBegin, key - keyBegin, valueBegin);
    }

  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                      public prefix/suffix methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the prefix path of the request
////////////////////////////////////////////////////////////////////////////////

char const* HttpRequest::prefix () const {
  return _prefix.c_str();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the path of the request
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setPrefix (char const* path) {
  _prefix = path;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns all suffix parts
////////////////////////////////////////////////////////////////////////////////

vector<string> const& HttpRequest::suffix () const {
  return _suffix;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a suffix part
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::addSuffix (char const* part) {
  string decoded = StringUtils::urlDecode(part);
  size_t tmpLength = 0;
  char* utf8_nfc = TRI_normalize_utf8_to_NFC(TRI_UNKNOWN_MEM_ZONE, decoded.c_str(), decoded.length(), &tmpLength);
  if (utf8_nfc) {
    _suffix.push_back(utf8_nfc);
    TRI_Free(TRI_UNKNOWN_MEM_ZONE, utf8_nfc);
  }
  else {
    _suffix.push_back(decoded);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set the request context
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setRequestContext (RequestContext* requestContext,
                                     bool isRequestContextOwner) {
  if (_requestContext) {
    // if we have a shared context, we should not have got here
    assert(isRequestContextOwner);

    // delete any previous context
    assert(false);
    delete _requestContext;
  }

  _requestContext        = requestContext;
  _isRequestContextOwner = isRequestContextOwner;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                             public static methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief translate the HTTP protocol version
////////////////////////////////////////////////////////////////////////////////

string HttpRequest::translateVersion (HttpVersion version) {
  switch (version) {
    case HTTP_1_0:      return "HTTP/1.0";
    case HTTP_1_1:      return "HTTP/1.1";
    case HTTP_UNKNOWN:   
    default:            return "HTTP/1.0";
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief translate an enum value into an HTTP method string
////////////////////////////////////////////////////////////////////////////////

string HttpRequest::translateMethod (const HttpRequestType method) {
  if (method == HTTP_REQUEST_DELETE) {
    return "DELETE";
  }
  else if (method == HTTP_REQUEST_GET) {
    return "GET";
  }
  else if (method == HTTP_REQUEST_HEAD) {
    return "HEAD";
  }
  else if (method == HTTP_REQUEST_OPTIONS) {
    return "OPTIONS";
  }
  else if (method == HTTP_REQUEST_PATCH) {
    return "PATCH";
  }
  else if (method == HTTP_REQUEST_POST) {
    return "POST";
  }
  else if (method == HTTP_REQUEST_PUT) {
    return "PUT";
  }

  LOGGER_WARNING("illegal http request method encountered in switch");
  return "UNKNOWN";
}

////////////////////////////////////////////////////////////////////////////////
/// @brief translate an HTTP method string into an enum value
////////////////////////////////////////////////////////////////////////////////

HttpRequest::HttpRequestType HttpRequest::translateMethod (const string& method) {
  const string methodString = StringUtils::toupper(method);

  if (methodString == "DELETE") {
    return HTTP_REQUEST_DELETE;
  }
  else if (methodString == "GET") {
    return HTTP_REQUEST_GET;
  }
  else if (methodString == "HEAD") {
    return HTTP_REQUEST_HEAD;
  }
  else if (methodString == "OPTIONS") {
    return HTTP_REQUEST_OPTIONS;
  }
  else if (methodString == "PATCH") {
    return HTTP_REQUEST_PATCH;
  }
  else if (methodString == "POST") {
    return HTTP_REQUEST_POST;
  }
  else if (methodString == "PUT") {
    return HTTP_REQUEST_PUT;
  }

  return HTTP_REQUEST_ILLEGAL;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief append the request method string to a string buffer
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::appendMethod (HttpRequestType method, StringBuffer* buffer) {
  buffer->appendText(translateMethod(method));
  buffer->appendChar(' ');
}

////////////////////////////////////////////////////////////////////////////////
/// @brief the expected content-type for a subpart
////////////////////////////////////////////////////////////////////////////////

const string& HttpRequest::getPartContentType () {
  static const string contentType = "application/x-arango-batchpart";

  return contentType;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief the expected content-type for a multipart message
////////////////////////////////////////////////////////////////////////////////

const string& HttpRequest::getMultipartContentType () {
  static const string contentType = "multipart/form-data";

  return contentType;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set array value
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setArrayValue (char* key, size_t length, char const* value) {
  Dictionary< vector<char const*>* >::KeyValue const* kv = _arrayValues.lookup(key);
  vector<char const*>* v = 0;

  if (kv == 0) {
    v = new vector<char const*>;
    _arrayValues.insert(key, length, v);
  }
  else {
    v = kv->_value;
  }

  v->push_back(value);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set cookie
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::setCookie (char* key, size_t length, char const* value) {
  _cookies.insert(key, length, value);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parse value of a cookie header field
////////////////////////////////////////////////////////////////////////////////

void HttpRequest::parseCookies (const char* buffer) {
  char* keyBegin = 0;
  char* key = 0;

  char* valueBegin = 0;
  char* value = 0;

  enum { KEY, VALUE } phase = KEY;
  enum { NORMAL, HEX1, HEX2 } reader = NORMAL;

  int hex = 0;

  char const AMB     = ';';
  char const EQUAL   = '=';
  char const PERCENT = '%';
  char const SPACE   = ' ';

  char* buffer2 = (char*) buffer;
  char* end = buffer2 + strlen(buffer);

  for (keyBegin = key = buffer2; buffer2 < end; buffer2++) {
    char next = *buffer2;

    if (phase == KEY && next == EQUAL) {
      phase = VALUE;

      valueBegin = value = buffer2 + 1;

      continue;
    }
    else if (next == AMB) {
      phase = KEY;

      *key = '\0';

      // check for missing value phase
      if (valueBegin == 0) {
        valueBegin = value = key;
      }
      else {
        *value = '\0';
      }

      setCookie(keyBegin, key - keyBegin, valueBegin);

      //keyBegin = key = buffer2 + 1;
      while ( *(keyBegin = key = buffer2 + 1) == SPACE && buffer2 < end) {
        buffer2++;
      }
      valueBegin = value = 0;

      continue;
    }
    else if (next == PERCENT) {
      reader = HEX1;
      continue;
    }
    else if (reader == HEX1) {
      int h1 = StringUtils::hex2int(next, -1);

      if (h1 == -1) {
        reader = NORMAL;
        --buffer2;
        continue;
      }

      hex = h1 * 16;
      reader = HEX2;
      continue;
    }
    else if (reader == HEX2) {
      int h1 = StringUtils::hex2int(next, -1);

      if (h1 == -1) {
        --buffer2;
      }
      else {
        hex += h1;
      }

      reader = NORMAL;
      next = static_cast<char>(hex);
    }

    if (phase == KEY) {
      *key++ = next;
    }
    else {
      *value++ = next;
    }
  }

  if (keyBegin != key) {
    *key = '\0';

    // check for missing value phase
    if (valueBegin == 0) {
      valueBegin = value = key;
    }
    else {
      *value = '\0';
    }

    setCookie(keyBegin, key - keyBegin, valueBegin);
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
