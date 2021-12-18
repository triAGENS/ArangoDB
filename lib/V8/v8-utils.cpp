////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////


#include "v8-utils.h"

#include "Basics/Common.h"
#include "Basics/operating-system.h"

#ifdef _WIN32
#include <WinSock2.h>  // must be before windows.h
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <windef.h>
#include <windows.h>
#include "Basics/win-utils.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicode/umachine.h>
#include <unicode/unistr.h>
#include <unicode/unorm2.h>
#include <unicode/utypes.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "unicode/normalizer2.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "ApplicationFeatures/HttpEndpointProvider.h"
#include "ApplicationFeatures/V8SecurityFeature.h"
#include "Basics/Exceptions.h"
#include "Basics/FileResultString.h"
#include "Basics/FileUtils.h"
#include "Basics/Nonce.h"
#include "Basics/PhysicalMemory.h"
#include "Basics/Result.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringBuffer.h"
#include "Basics/StringUtils.h"
#include "Basics/Thread.h"
#include "Basics/Utf8Helper.h"
#include "Basics/build.h"
#include "Basics/debugging.h"
#include "Basics/error.h"
#include "Basics/files.h"
#include "Basics/memory.h"
#include "Basics/process-utils.h"
#include "Basics/signals.h"
#include "Basics/socket-utils.h"
#include "Basics/system-compiler.h"
#include "Basics/system-functions.h"
#include "Basics/terminal-utils.h"
#include "Basics/threads.h"
#include "Basics/tri-strings.h"
#include "Basics/tri-zip.h"
#include "Basics/voc-errors.h"
#include "Endpoint/Endpoint.h"
#include "Logger/LogLevel.h"
#include "Logger/LogMacros.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Random/UniformCharacter.h"
#include "Rest/CommonDefines.h"
#include "Rest/GeneralRequest.h"
#include "Rest/GeneralResponse.h"
#include "SimpleHttpClient/GeneralClientConnection.h"
#include "SimpleHttpClient/SimpleHttpClient.h"
#include "SimpleHttpClient/SimpleHttpResult.h"
#include "Ssl/SslInterface.h"
#include "Ssl/ssl-helper.h"
#include "Utilities/NameValidator.h"
#include "V8/v8-buffer.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-vpack.h"
#include "V8/v8-deadline.h"

#ifdef TRI_HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <velocypack/Validator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::httpclient;
using namespace arangodb::rest;

/// @brief Random string generators
namespace {
static UniformCharacter JSAlphaNumGenerator(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
static UniformCharacter JSNumGenerator("0123456789");
static UniformCharacter JSSaltGenerator(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*(){}"
    "[]:;<>,.?/|");

arangodb::Result doSleep(double n, arangodb::application_features::ApplicationServer& server) {
  double until = TRI_microtime() + n;

  while (true) {
    if (server.isStopping()) {
      return {TRI_ERROR_SHUTTING_DOWN};
    }

    double now = TRI_microtime();
    if (now >= until) {
      return {};
    }
    uint64_t duration =
        (until - now >= 0.5) ? 100000 : static_cast<uint64_t>((until - now) * 1000000);

    std::this_thread::sleep_for(std::chrono::microseconds(duration));
  }
}

}  // namespace

/// @brief Converts an object to a UTF-8-encoded and normalized character array.
TRI_Utf8ValueNFC::TRI_Utf8ValueNFC(v8::Isolate* isolate, v8::Handle<v8::Value> const obj)
    : _str(nullptr), _length(0) {
  v8::String::Value str(isolate, obj);

  _str = TRI_normalize_utf16_to_NFC(*str, (size_t)str.length(), &_length);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Destroys a normalized string object
////////////////////////////////////////////////////////////////////////////////

TRI_Utf8ValueNFC::~TRI_Utf8ValueNFC() { TRI_Free(_str); }

////////////////////////////////////////////////////////////////////////////////
/// @brief create a JavaScript error object
////////////////////////////////////////////////////////////////////////////////

static void CreateErrorObject(v8::Isolate* isolate, ErrorCode errorNumber,
                              std::string_view message) noexcept {
  try {
    TRI_GET_GLOBALS();
    if (errorNumber == TRI_ERROR_OUT_OF_MEMORY) {
      LOG_TOPIC("532c3", ERR, arangodb::Logger::FIXME)
        << "encountered out-of-memory error in context #" << v8g->_id;
    }

    v8::Handle<v8::String> errorMessage = TRI_V8_STD_STRING(isolate, message);

    if (errorMessage.IsEmpty()) {
      isolate->ThrowException(v8::Object::New(isolate));
      return;
    }

    v8::Handle<v8::Value> err = v8::Exception::Error(errorMessage);

    if (err.IsEmpty()) {
      isolate->ThrowException(v8::Object::New(isolate));
      return;
    }

    v8::Handle<v8::Object> errorObject =
        err->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());

    if (errorObject.IsEmpty()) {
      isolate->ThrowException(v8::Object::New(isolate));
      return;
    }

    auto context = TRI_IGETC;
    errorObject
        ->Set(context, TRI_V8_STD_STRING(isolate, StaticStrings::ErrorNum),
              v8::Number::New(isolate, int(errorNumber)))
        .FromMaybe(false);
    errorObject->Set(context, TRI_V8_STD_STRING(isolate, StaticStrings::ErrorMessage), errorMessage).FromMaybe(false);

    TRI_GET_GLOBAL(ArangoErrorTempl, v8::ObjectTemplate);

    v8::Handle<v8::Object> ArangoError = ArangoErrorTempl->NewInstance(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());

    if (!ArangoError.IsEmpty()) {
      errorObject->SetPrototype(TRI_IGETC, ArangoError).FromMaybe(false);  // Ignore error
    }

    isolate->ThrowException(errorObject);
  } catch (...) {
    // must not throw from here, as a C++ exception must not escape the
    // C++ bindings that are called by V8. if it does, the program will crash
  }
}


////////////////////////////////////////////////////////////////////////////////
/// @brief reads/execute a file into/in the current context
////////////////////////////////////////////////////////////////////////////////

static bool LoadJavaScriptFile(v8::Isolate* isolate, char const* filename,
                               bool execute) {
  v8::HandleScope handleScope(isolate);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, filename, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + filename);
  }

  size_t length;
  char* content = TRI_SlurpFile(filename, &length);

  if (content == nullptr) {
    LOG_TOPIC("790a5", ERR, arangodb::Logger::FIXME)
        << "cannot load JavaScript file '" << filename << "': " << TRI_last_error();
    return false;
  }

  auto guard = scopeGuard([&content]() noexcept { TRI_FreeString(content); });

  if (content == nullptr) {
    LOG_TOPIC("89c6f", ERR, arangodb::Logger::FIXME)
        << "cannot load JavaScript file '" << filename
        << "': " << TRI_errno_string(TRI_ERROR_OUT_OF_MEMORY);
    return false;
  }

  v8::Handle<v8::String> name = TRI_V8_STRING(isolate, filename);
  v8::Handle<v8::String> source =
      TRI_V8_PAIR_STRING(isolate, content, (int)length);

  v8::TryCatch tryCatch(isolate);

  v8::ScriptOrigin scriptOrigin(name);
  v8::Handle<v8::Script> script =
      v8::Script::Compile(TRI_IGETC, source, &scriptOrigin).FromMaybe(v8::Local<v8::Script>());

  if (tryCatch.HasCaught()) {
    TRI_LogV8Exception(isolate, &tryCatch);
    return false;
  }

  // compilation failed, print errors that happened during compilation
  if (script.IsEmpty()) {
    LOG_TOPIC("6fffa", ERR, arangodb::Logger::FIXME)
        << "cannot load JavaScript file '" << filename << "': compilation failed.";
    return false;
  }

  if (execute) {
    // execute script
    v8::Handle<v8::Value> result = script->Run(TRI_IGETC).FromMaybe(
        v8::Local<v8::Value>());  // TODO: do we want a better default fail here?

    if (tryCatch.HasCaught()) {
      TRI_LogV8Exception(isolate, &tryCatch);
      return false;
    }

    if (result.IsEmpty()) {
      return false;
    }
  }

  LOG_TOPIC("fe6a4", TRACE, arangodb::Logger::FIXME)
      << "loaded JavaScript file: '" << filename << "'";
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the program options
////////////////////////////////////////////////////////////////////////////////

static void JS_Options(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("options()");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  auto filter = [&v8security, isolate](std::string const& name) {
    if (name.find("passwd") != std::string::npos ||
        name.find("password") != std::string::npos ||
        name.find("secret") != std::string::npos) {
      return false;
    }
    return v8security.shouldExposeStartupOption(isolate, name);
  };

  VPackBuilder builder = v8g->_server.options(filter);

  auto result = TRI_VPackToV8(isolate, builder.slice());

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief decodes a base64-encoded string
///
/// @FUN{internal.base64Decode(@FA{value})}
///
/// Base64-decodes the string @FA{value}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Base64Decode(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("base64Decode(<value>)");
  }

  try {
    std::string const value = TRI_ObjectToString(isolate, args[0]);
    std::string const base64 = StringUtils::decodeBase64(value);

    TRI_V8_RETURN_STD_STRING(base64);
  } catch (...) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  TRI_ASSERT(false);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief encodes a string as base64
///
/// @FUN{internal.base64Encode(@FA{value})}
///
/// Base64-encodes the string @FA{value}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Base64Encode(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("base64Encode(<value>)");
  }

  try {
    std::string const&& value = TRI_ObjectToString(isolate, args[0]);
    std::string const&& base64 = StringUtils::encodeBase64(value);

    TRI_V8_RETURN_STD_STRING(base64);
  } catch (...) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  TRI_ASSERT(false);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a JavaScript snippet, but does not execute it
///
/// @FUN{internal.parse(@FA{script})}
///
/// Parses the @FA{script} code, but does not execute it.
/// Will return *true* if the code does not have a parse error, and throw
/// an exception otherwise.
////////////////////////////////////////////////////////////////////////////////

static void JS_Parse(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("parse(<script>)");
  }

  v8::Handle<v8::Value> source = args[0];
  v8::Handle<v8::Value> filename;

  if (args.Length() > 1) {
    filename = args[1];
  } else {
    filename = TRI_V8_ASCII_STRING(isolate, "<snippet>");
  }

  if (!source->IsString()) {
    TRI_V8_THROW_TYPE_ERROR("<script> must be a string");
  }

  v8::TryCatch tryCatch(isolate);

  v8::ScriptOrigin scriptOrigin(TRI_ObjectToString(context, filename));
  v8::Handle<v8::Script> script =
      v8::Script::Compile(TRI_IGETC,
                          source->ToString(TRI_IGETC).FromMaybe(v8::Local<v8::String>()),
                          &scriptOrigin)
          .FromMaybe(v8::Handle<v8::Script>());

  // compilation failed, we have caught an exception
  if (tryCatch.HasCaught()) {
    if (tryCatch.CanContinue()) {
      v8::Local<v8::Object> exceptionObj = tryCatch.Exception().As<v8::Object>();
      v8::Handle<v8::Message> message = tryCatch.Message();
      if (!message.IsEmpty()) {
        exceptionObj->Set(context,
                          TRI_V8_ASCII_STRING(isolate, "lineNumber"),
                          v8::Number::New(isolate,
                                          message->GetLineNumber(TRI_IGETC).FromMaybe(-1))).FromMaybe(false);
        exceptionObj->Set(context,
                          TRI_V8_ASCII_STRING(isolate, "columnNumber"),
                          v8::Number::New(isolate, message->GetStartColumn())).FromMaybe(false);
      }
      exceptionObj->Set(context,
                        TRI_V8_ASCII_STRING(isolate, "fileName"),
                        filename->ToString(TRI_IGETC).FromMaybe(
                            TRI_V8_ASCII_STRING(isolate, "unknown"))).FromMaybe(false);
      tryCatch.ReThrow();
      return;
    } else {
      TRI_GET_GLOBALS();
      v8g->_canceled = true;
      tryCatch.ReThrow();
      return;
    }
  }

  // compilation failed, we don't know why
  if (script.IsEmpty()) {
    TRI_V8_RETURN_FALSE();
  }
  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a JavaScript file, but does not execute it
///
/// @FUN{internal.parseFile(@FA{filename})}
///
/// Parses the file @FA{filename}, but does not execute it.
/// Will return *true* if the code does not have a parse error, and throw
/// an exception otherwise.
////////////////////////////////////////////////////////////////////////////////

static void JS_ParseFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("parseFile(<filename>)");
  }

  if (!args[0]->IsString() && !args[0]->IsStringObject()) {
    TRI_V8_THROW_EXCEPTION_USAGE("parseFile(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  size_t length;
  char* content = TRI_SlurpFile(*name, &length);

  if (content == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), "cannot read file");
  }

  v8::TryCatch tryCatch(isolate);

  v8::ScriptOrigin scriptOrigin(TRI_ObjectToString(context, args[0]));
  v8::Handle<v8::Script> script =
      v8::Script::Compile(TRI_IGETC, TRI_V8_PAIR_STRING(isolate, content, (int)length), &scriptOrigin)
          .FromMaybe(v8::Handle<v8::Script>());

  TRI_FreeString(content);

  // compilation failed, we have caught an exception
  if (tryCatch.HasCaught()) {
    if (tryCatch.CanContinue()) {
      v8::Local<v8::Object> exceptionObj = tryCatch.Exception().As<v8::Object>();
      v8::Handle<v8::Message> message = tryCatch.Message();
      if (!message.IsEmpty()) {
        exceptionObj->Set(context,
                          TRI_V8_ASCII_STRING(isolate, "lineNumber"),
                          v8::Number::New(isolate,
                                          message->GetLineNumber(TRI_IGETC).FromMaybe(-1))).FromMaybe(false);
        exceptionObj->Set(context,
                          TRI_V8_ASCII_STRING(isolate, "columnNumber"),
                          v8::Number::New(isolate, message->GetStartColumn())).FromMaybe(false);
      }
      exceptionObj->Set(context, TRI_V8_ASCII_STRING(isolate, "fileName"), args[0]).FromMaybe(false);
      tryCatch.ReThrow();
      return;
    } else {
      TRI_GET_GLOBALS();
      v8g->_canceled = true;
      tryCatch.ReThrow();
      return;
    }
  }

  // compilation failed, we don't know why
  if (script.IsEmpty()) {
    TRI_V8_RETURN_FALSE();
  }
  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief helper function for internal.download()
////////////////////////////////////////////////////////////////////////////////

static std::string GetEndpointFromUrl(std::string const& url) {
  char const* p = url.c_str();
  char const* e = p + url.size();
  size_t slashes = 0;

  while (p < e) {
    if (*p == '?') {
      // http(s)://example.com?foo=bar
      return url.substr(0, p - url.c_str());
    } else if (*p == '/') {
      if (++slashes == 3) {
        return url.substr(0, p - url.c_str());
      }
    }
    ++p;
  }

  return url;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief downloads data from a URL
///
/// @FUN{internal.download(@FA{url}, @FA{body}, @FA{options}, @FA{outfile})}
///
/// Downloads the data from the URL specified by @FA{url} and saves the
/// response body to @FA{outfile}. The following @FA{options} are supported:
///
/// - @LIT{method}: the HTTP method to be used. The supported HTTP methods are
///   @LIT{DELETE}, @LIT{GET}, @LIT{HEAD}, @LIT{POST}, @LIT{PUT}, @LIT{PATCH}
///
/// - @LIT{timeout}: a timeout value for the connection
///
/// - @LIT{followRedirects}: whether or not to follow redirects
///
/// - @LIT{maxRedirects}: maximum number of redirects to follow, only useful
///   if @LIT{followRedirects} is *true*.
///
/// - @LIT{returnBodyOnError}: whether or not to return / save body on HTTP
///   error
///
/// - @LIT{returnBodyAsBuffer}: whether or not to return the result body in
///   a Buffer object (default is false, body will be returned in a String)
///
/// - @LIT{headers}: an optional array of headers to be sent for the first
///   (non-redirect) request.
///
/// Up to 5 redirects will be followed. Any user-defined headers will only be
/// sent for the first request. If no timeout is given, a default timeout will
/// be used.
///
/// If @FA{outfile} is specified, the result body will be saved in a file
/// specified by @FA{outfile}. If @FA{outfile} already exists, an error will
/// be thrown.
///
/// If @FA{outfile} is not specified, the result body will be returned in the
/// @LIT{body} attribute of the result object.
///
/// `process-utils.js` depends on simple http client error messages.
///   this needs to be adjusted if this is ever changed!
////////////////////////////////////////////////////////////////////////////////
namespace {

auto getEndpoint(v8::Isolate* isolate, std::vector<std::string> const& endpoints,
                 std::string& url, std::string& lastEndpoint)
    -> std::tuple<std::string, std::string, std::string> {
  // returns endpoint, relative, error
  std::string relative;
  std::string endpoint;
  if (url.substr(0, 7) == "http://") {
    endpoint = GetEndpointFromUrl(url).substr(7);
    relative = url.substr(7 + endpoint.length());

    if (relative.empty() || relative[0] != '/') {
      relative = "/" + relative;
    }
    if (endpoint.find(':') == std::string::npos) {
      endpoint.append(":80");
    }
    endpoint = "tcp://" + endpoint;
  } else if (url.substr(0, 8) == "https://") {
    endpoint = GetEndpointFromUrl(url).substr(8);
    relative = url.substr(8 + endpoint.length());

    if (relative.empty() || relative[0] != '/') {
      relative = "/" + relative;
    }
    if (endpoint.find(':') == std::string::npos) {
      endpoint.append(":443");
    }
    endpoint = "ssl://" + endpoint;
  } else if (url.substr(0, 5) == "h2://") {
    endpoint = GetEndpointFromUrl(url).substr(5);
    relative = url.substr(5 + endpoint.length());

    if (relative.empty() || relative[0] != '/') {
      relative = "/" + relative;
    }
    if (endpoint.find(':') == std::string::npos) {
      endpoint.append(":80");
    }
    endpoint = "tcp://" + endpoint;
  } else if (url.substr(0, 6) == "h2s://") {
    endpoint = GetEndpointFromUrl(url).substr(6);
    relative = url.substr(6 + endpoint.length());

    if (relative.empty() || relative[0] != '/') {
      relative = "/" + relative;
    }
    if (endpoint.find(':') == std::string::npos) {
      endpoint.append(":443");
    }
    endpoint = "ssl://" + endpoint;
  } else if (url.substr(0, 6) == "srv://") {
    size_t found = url.find('/', 6);

    relative = "/";
    if (found != std::string::npos) {
      relative.append(url.substr(found + 1));
      endpoint = url.substr(6, found - 6);
    } else {
      endpoint = url.substr(6);
    }
    endpoint = "srv://" + endpoint;
  } else if (url.substr(0, 7) == "unix://") {
    // Can only have arrived here if endpoints is non empty
    if (endpoints.empty()) {
      return {"", "", std::move("unsupported URL specified")};
    }
    endpoint = endpoints.front();
    relative = url.substr(endpoint.size());
  } else if (!url.empty() && url[0] == '/') {
    size_t found;
    // relative URL. prefix it with last endpoint
    relative = url;
    url = lastEndpoint + url;
    endpoint = lastEndpoint;
    if (endpoint.substr(0, 5) == "http:") {
      endpoint = endpoint.substr(5);
      found = endpoint.find(":");
      if (found == std::string::npos) {
        endpoint = endpoint + ":80";
      }
      endpoint = "tcp:" + endpoint;
    } else if (endpoint.substr(0, 6) == "https:") {
      endpoint = endpoint.substr(6);
      found = endpoint.find(":");
      if (found == std::string::npos) {
        endpoint = endpoint + ":443";
      }
      endpoint = "ssl:" + endpoint;
    }
  } else {
    std::string msg("unsupported URL specified: '");
    msg.append(url).append("'");
    return {"", "", std::move(msg)};
  }
  return {std::move(endpoint), std::move(relative), ""};
}
}  // namespace

void JS_Download(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::HandleScope scope(isolate);

  if (isExecutionDeadlineReached(isolate)) {
    return;
  }
  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  std::string const signature = "download(<url>, <body>, <options>, <outfile>)";

  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE(signature);
  }

  std::string const inputUrl = TRI_ObjectToString(isolate, args[0]);
  std::string url = inputUrl;
  std::vector<std::string> endpoints;

  bool isLocalUrl = false;

  if (!url.empty() && url[0] == '/') {
    // check if we are a server
    isLocalUrl = true;
    TRI_ASSERT(v8g->_server.hasFeature<HttpEndpointProvider>());
    auto& feature = v8g->_server.getFeature<HttpEndpointProvider>();
    endpoints = feature.httpEndpoints();

    // a relative url. now make this an absolute URL if possible
    for (auto const& endpoint : endpoints) {
      std::string fullurl = endpoint;

      // ipv4: replace 0.0.0.0 with 127.0.0.1
      auto pos = fullurl.find("//0.0.0.0");

      if (pos != std::string::npos) {
        fullurl.replace(pos, strlen("//0.0.0.0"), "//127.0.0.1");
      }

      // ipv6: replace [::] with [::1]
      else {
        pos = fullurl.find("//[::]");

        if (pos != std::string::npos) {
          // if the server does not support ipv6, the bind will still
          // succeed, but you need to connect to 127.0.0.1
          fullurl.replace(pos, strlen("//[::]"), "//127.0.0.1");
        }
      }

      url = fullurl + url;

      break;
    }
  }

  std::string lastEndpoint = GetEndpointFromUrl(url);

  std::string body;
  if (args.Length() > 1) {
    if (args[1]->IsString() || args[1]->IsStringObject()) {
      // supplied body is a string
      body = TRI_ObjectToString(isolate, args[1]);
    } else if (args[1]->IsObject() && V8Buffer::hasInstance(isolate, args[1])) {
      // supplied body is a Buffer object
      char const* data = V8Buffer::data(isolate, args[1].As<v8::Object>());
      size_t size = V8Buffer::length(isolate, args[1].As<v8::Object>());

      if (data == nullptr) {
        TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "invalid <body> buffer value");
      }

      body.assign(data, size);
    }
  }

  // options
  // ------------------------------------------------------------------------

  std::unordered_map<std::string, std::string> headerFields;
  double timeout = 10.0;
  bool returnBodyAsBuffer = false;
  bool followRedirects = true;
  rest::RequestType method = rest::RequestType::GET;
  bool returnBodyOnError = false;
  int maxRedirects = 5;
  uint64_t sslProtocol = TLS_V12;
  std::string jwtToken, username, password;

  if (args.Length() > 2) {
    if (!args[2]->IsObject()) {
      TRI_V8_THROW_EXCEPTION_USAGE(signature);
    }

    v8::Handle<v8::Array> options = v8::Handle<v8::Array>::Cast(args[2]);

    if (options.IsEmpty()) {
      TRI_V8_THROW_EXCEPTION_USAGE(signature);
    }

    // ssl protocol
    if (TRI_HasProperty(context, isolate, options, "sslProtocol")) {
      if (sslProtocol >= SSL_LAST) {
        // invalid protocol
        TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "invalid option value for sslProtocol");
      }

      sslProtocol = TRI_ObjectToUInt64(
          isolate, TRI_GetProperty(context, isolate, options, "sslProtocol"), false);
    }

    // method
    if (TRI_HasProperty(context, isolate, options, "method")) {
      std::string methodString =
          TRI_ObjectToString(isolate, TRI_GetProperty(context, isolate, options,
                                                      "method"));

      method = GeneralRequest::translateMethod(methodString);
    }

    // headers
    if (TRI_HasProperty(context, isolate, options, "headers")) {
      v8::Local<v8::Object> v8Headers = v8::Local<v8::Object>::Cast(options->Get(context, TRI_V8_ASCII_STRING(isolate, "headers")).FromMaybe(v8::Local<v8::Value>()));
      if (v8Headers->IsObject()) {
        v8::Handle<v8::Array> props = v8Headers->GetPropertyNames(TRI_IGETC).FromMaybe(v8::Handle<v8::Array>());

        for (uint32_t i = 0; i < props->Length(); i++) {
          v8::Handle<v8::Value> key = props->Get(context, i).FromMaybe(v8::Local<v8::Value>());
          headerFields[TRI_ObjectToString(isolate, key)] =
            TRI_ObjectToString(context, isolate, v8Headers->Get(context, key));
        }
      }
    }

    // timeout
    if (TRI_HasProperty(context, isolate, options, "timeout")) {
      v8::MaybeLocal<v8::Value> tv = options->Get(context, TRI_V8_ASCII_STRING(isolate, "timeout"));
      if (!tv.IsEmpty()) {
        v8::Local<v8::Value> v = tv.FromMaybe(v8::Local<v8::Value>());
        if (!v->IsNumber()) {
          TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                         "invalid option value for timeout");
        }

        timeout = TRI_ObjectToDouble(isolate, TRI_GetProperty(context, isolate,
                                                              options, "timeout"));
      }
    }
    timeout = correctTimeoutToExecutionDeadlineS(timeout);

    // return body as a buffer?
    if (TRI_HasProperty(context, isolate, options, "returnBodyAsBuffer")) {
      returnBodyAsBuffer =
          TRI_ObjectToBoolean(isolate, TRI_GetProperty(context, isolate, options,
                                                       "returnBodyAsBuffer"));
    }

    // follow redirects
    if (TRI_HasProperty(context, isolate, options, "followRedirects")) {
      followRedirects =
          TRI_ObjectToBoolean(isolate, TRI_GetProperty(context, isolate, options,
                                                       "followRedirects"));
    }

    // max redirects
    if (TRI_HasProperty(context, isolate, options, "maxRedirects")) {
      if (!TRI_GetProperty(context, isolate, options, "maxRedirects")->IsNumber()) {
        TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "invalid option value for maxRedirects");
      }

      maxRedirects =
          (int)TRI_ObjectToInt64(isolate, TRI_GetProperty(context, isolate, options,
                                                          "maxRedirects"));
    }

    if (!body.empty() &&
        (method == rest::RequestType::GET || method == rest::RequestType::HEAD)) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "should not provide a body value for this request method");
    }

    if (TRI_HasProperty(context, isolate, options, "returnBodyOnError")) {
      returnBodyOnError =
          TRI_ObjectToBoolean(isolate, TRI_GetProperty(context, isolate, options,
                                                       "returnBodyOnError"));
    }

    if (TRI_HasProperty(context, isolate, options, "jwt")) {
      jwtToken = TRI_ObjectToString(isolate, TRI_GetProperty(context, isolate,
                                                             options, "jwt"));
    } else if (TRI_HasProperty(context, isolate, options, "username")) {
      username = TRI_ObjectToString(isolate, TRI_GetProperty(context, isolate, options,
                                                             "username"));
      if (TRI_HasProperty(context, isolate, options, "password")) {
        password = TRI_ObjectToString(isolate, TRI_GetProperty(context, isolate, options,
                                                               "password"));
      }
    }
  }

  // outfile
  std::string outfile;
  if (args.Length() >= 4) {
    if (args[3]->IsString() || args[3]->IsStringObject()) {
      outfile = TRI_ObjectToString(isolate, args[3]);
    }

    if (outfile.empty()) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "invalid value provided for outfile");
    }

    if (!v8security.isAllowedToAccessPath(isolate, outfile, FSAccessType::WRITE)) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(
          TRI_ERROR_FORBIDDEN, std::string("not allowed to modify files in this path: ") + outfile);
    }

    if (TRI_ExistsFile(outfile.c_str())) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_CANNOT_OVERWRITE_FILE);
    }
  }

  int numRedirects = 0;

  while (numRedirects < maxRedirects) {

    auto [endpoint, relative, error] = getEndpoint(isolate, endpoints, url, lastEndpoint);
    if (!error.empty()) {
      TRI_V8_THROW_SYNTAX_ERROR(error.c_str());
    }

    LOG_TOPIC("d6bdb", TRACE, arangodb::Logger::FIXME)
      << "downloading file. endpoint: " << endpoint << ", relative URL: " << url;

    if (!isLocalUrl && !v8security.isAllowedToConnectToEndpoint(isolate, endpoint, inputUrl)) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                       "not allowed to connect to this URL: " + inputUrl);
    }

    std::unique_ptr<Endpoint> ep(Endpoint::clientFactory(endpoint));

    if (ep.get() == nullptr) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     std::string("invalid URL ") + url);
    }

    if (ep.get()->isBroadcastBind()) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     std::string("Cannot connect to INADDR_ANY or INADDR6_ANY ") + url);
    }

    std::unique_ptr<GeneralClientConnection> connection(
      GeneralClientConnection::factory(v8g->_server, ep.get(), timeout,
                                       timeout, 3, sslProtocol));

    if (connection == nullptr) {
      TRI_V8_THROW_EXCEPTION_MEMORY();
    }

    SimpleHttpClientParams params(timeout, false);
    params.setSupportDeflate(false);
    // security by obscurity won't work. Github requires a useragent nowadays.
    params.setExposeArangoDB(true);
    if (!jwtToken.empty()) {
      params.setJwt(jwtToken);
    } else if (!username.empty()) {
      params.setUserNamePassword("/", username, password);
    }
    SimpleHttpClient client(connection.get(), params);

    v8::Handle<v8::Object> result = v8::Object::New(isolate);

    if (numRedirects > 0) {
      // do not send extra headers now
      headerFields.clear();
    }

    // send the actual request
    std::unique_ptr<SimpleHttpResult> response(
        client.request(method, relative, (body.size() > 0 ? body.c_str() : ""),
                       body.size(), headerFields));

    int returnCode = 500;  // set a default
    std::string returnMessage = "";

    if (response == nullptr || !response->isComplete()) {
      // save error message
      returnMessage = client.getErrorMessage();
      returnCode = 500;

      if (response != nullptr && response->getHttpReturnCode() > 0) {
        returnCode = response->getHttpReturnCode();
      }
    } else {
      TRI_ASSERT(response != nullptr);

      returnMessage = response->getHttpReturnMessage();
      returnCode = response->getHttpReturnCode();

      // follow redirects?
      if (followRedirects && (returnCode == 301 || returnCode == 302 || returnCode == 307)) {
        bool found;
        url = response->getHeaderField(StaticStrings::Location, found);

        if (!found) {
          TRI_V8_THROW_EXCEPTION_INTERNAL("caught invalid redirect URL");
        }

        numRedirects++;

        isLocalUrl = false;
        if (!url.empty() && url[0] == '/') {
          isLocalUrl = true;
        }
        if (url.substr(0, 5) == "http:" || url.substr(0, 6) == "https:") {
          lastEndpoint = GetEndpointFromUrl(url);
        }
        continue;
      }

      // process response headers
      auto const& responseHeaders = response->getHeaderFields();

      v8::Handle<v8::Object> headers = v8::Object::New(isolate);

      for (auto const& it : responseHeaders) {
        headers->Set(context,
                     TRI_V8_STD_STRING(isolate, it.first),
                     TRI_V8_STD_STRING(isolate, it.second)).FromMaybe(false);
      }

      result->Set(context, TRI_V8_ASCII_STRING(isolate, "headers"), headers).FromMaybe(false);

      if (returnBodyOnError || (returnCode >= 200 && returnCode <= 299)) {
        try {
          std::string json;
          basics::StringBuffer const& sb = response->getBody();
          std::string_view body(sb.c_str(), sb.length());

          bool found = false;
          std::string content =
              response->getHeaderField(StaticStrings::ContentTypeHeader, found);
          if (found && content.find(StaticStrings::MimeTypeVPack) != std::string::npos) {
            VPackOptions validationOptions = VPackOptions::Defaults;  // intentional copy
            validationOptions.validateUtf8Strings = true;
            validationOptions.checkAttributeUniqueness = true;
            validationOptions.disallowExternals = true;
            validationOptions.disallowCustom = true;
            velocypack::Validator validator(&validationOptions);
            validator.validate(sb.data(), sb.length());  // throws on error
            json.assign(VPackSlice(reinterpret_cast<uint8_t const*>(sb.data())).toJson());
            body = std::string_view(json);
          }

          if (outfile.size() > 0) {
            // save outfile
            FileUtils::spit(outfile, body.data(), body.length());
          } else {
            // set "body" attribute in result
            if (returnBodyAsBuffer) {
              V8Buffer* buffer = V8Buffer::New(isolate, body.data(), body.length());
              v8::Local<v8::Object> bufferObject =
                  v8::Local<v8::Object>::New(isolate, buffer->_handle);
              result->Set(context, TRI_V8_ASCII_STRING(isolate, "body"), bufferObject).FromMaybe(false);
            } else {
              result->Set(context,
                          TRI_V8_ASCII_STRING(isolate, "body"),
                          TRI_V8_STD_STRING(isolate, sb)).FromMaybe(false);
            }
          }

        } catch (...) {
        }
      }
    }

    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "code"),
                v8::Number::New(isolate, returnCode)).FromMaybe(false);
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "message"),
                TRI_V8_STD_STRING(isolate, returnMessage)).FromMaybe(false);

    TRI_V8_RETURN(result);
  }

  TRI_V8_THROW_EXCEPTION_INTERNAL("too many redirects");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a script
///
/// @FUN{internal.execute(@FA{script}, @FA{sandbox}, *filename*)}
///
/// Executes the @FA{script} with the @FA{sandbox} as context. Global variables
/// assigned inside the @FA{script}, will be visible in the @FA{sandbox} object
/// after execution. The *filename* is used for displaying error
/// messages.
///
/// If @FA{sandbox} is undefined, then @FN{execute} uses the current context.
////////////////////////////////////////////////////////////////////////////////

static void JS_Execute(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 3) {
    TRI_V8_THROW_EXCEPTION_USAGE("execute(<script>, <sandbox>, <filename>)");
  }

  v8::Handle<v8::Value> source = args[0];
  v8::Handle<v8::Value> sandboxValue = args[1];
  v8::Handle<v8::Value> filename = args[2];

  if (!source->IsString()) {
    TRI_V8_THROW_TYPE_ERROR("<script> must be a string");
  }

  bool useSandbox = sandboxValue->IsObject();
  v8::Handle<v8::Object> sandbox;
  v8::Handle<v8::Context> context = TRI_IGETC;

  if (useSandbox) {
    sandbox = sandboxValue->ToObject(context).FromMaybe(v8::Local<v8::Object>());

    // create new context
    context = v8::Context::New(isolate);
    context->Enter();

    // copy sandbox into context
    v8::Handle<v8::Array> keys = sandbox->GetPropertyNames(context).FromMaybe(v8::Local<v8::Array>());

    for (uint32_t i = 0; i < keys->Length(); i++) {
      v8::Handle<v8::String> key =
        TRI_ObjectToString(context, keys->Get(context, v8::Integer::New(isolate, i)).FromMaybe(v8::Handle<v8::Value>()));
      v8::Handle<v8::Value> value = sandbox->Get(context, key).FromMaybe(v8::Local<v8::Value>());

      if (Logger::logLevel() == arangodb::LogLevel::TRACE) {
        TRI_Utf8ValueNFC keyName(isolate, key);

        if (*keyName != nullptr) {
          LOG_TOPIC("1a894", TRACE, arangodb::Logger::FIXME)
              << "copying key '" << *keyName << "' from sandbox to context";
        }
      }

      if (value == sandbox) {
        value = context->Global();
      }
      context->Global()->Set(context, key, value).FromMaybe(false);
    }
  }

  // execute script inside the context
  v8::Handle<v8::Script> script;
  v8::Handle<v8::Value> result;

  {
    v8::TryCatch tryCatch(isolate);

    v8::ScriptOrigin scriptOrigin(TRI_ObjectToString(context, filename));
    script = v8::Script::Compile(context, TRI_ObjectToString(context, source), &scriptOrigin)
                 .FromMaybe(v8::Local<v8::Script>());

    // compilation failed, print errors that happened during compilation
    if (script.IsEmpty()) {
      if (useSandbox) {
        context->DetachGlobal();
        context->Exit();
      }

      if (tryCatch.CanContinue()) {
        v8::Local<v8::Object> exceptionObj = tryCatch.Exception().As<v8::Object>();
        v8::Handle<v8::Message> message = tryCatch.Message();
        if (!message.IsEmpty()) {
          exceptionObj->Set(context,
                            TRI_V8_ASCII_STRING(isolate, "lineNumber"),
                            v8::Number::New(isolate,
                                            message->GetLineNumber(context).FromMaybe(-1))).FromMaybe(false);
          exceptionObj->Set(context,
                            TRI_V8_ASCII_STRING(isolate, "columnNumber"),
                            v8::Number::New(isolate,
                                            message->GetStartColumn(context).FromMaybe(-1))).FromMaybe(false);
        }
        exceptionObj->Set(context,
                          TRI_V8_ASCII_STRING(isolate, "fileName"),
                          TRI_ObjectToString(context, filename)).FromMaybe(false);
        tryCatch.ReThrow();
        return;
      } else {
        TRI_GET_GLOBALS();
        v8g->_canceled = true;
        tryCatch.ReThrow();
        return;
      }
    }

    // compilation succeeded, run the script
    result = script->Run(context).FromMaybe(v8::Local<v8::Value>());

    if (result.IsEmpty()) {
      if (useSandbox) {
        context->DetachGlobal();
        context->Exit();
      }

      if (tryCatch.CanContinue()) {
        TRI_V8_LOG_THROW_EXCEPTION(tryCatch);
      } else {
        tryCatch.ReThrow();
        TRI_GET_GLOBALS();
        v8g->_canceled = true;
        TRI_V8_RETURN_UNDEFINED();
      }
    }
  }

  // copy result back into the sandbox
  if (useSandbox) {
    v8::Handle<v8::Array> keys = context->Global()->GetPropertyNames(context).FromMaybe(v8::Local<v8::Array>());

    for (uint32_t i = 0; i < keys->Length(); i++) {
      v8::Handle<v8::String> key =
        TRI_ObjectToString(context, keys->Get(context, v8::Integer::New(isolate, i)).FromMaybe(v8::Local<v8::Value>()));
      v8::Handle<v8::Value> value = context->Global()->Get(context, key).FromMaybe(v8::Handle<v8::Value>());

      if (Logger::logLevel() == arangodb::LogLevel::TRACE) {
        TRI_Utf8ValueNFC keyName(isolate, key);

        if (*keyName != nullptr) {
          LOG_TOPIC("a9a45", TRACE, arangodb::Logger::FIXME)
              << "copying key '" << *keyName << "' from context to sandbox";
        }
      }

      if (value == context->Global()) {
        value = sandbox;
      }

      sandbox->Set(context, key, value).FromMaybe(false);
    }

    context->DetachGlobal();
    context->Exit();
  }

  if (useSandbox) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Exists
////////////////////////////////////////////////////////////////////////////////

static void JS_Exists(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("exists(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  if (TRI_ExistsFile(*name)) {
    TRI_V8_RETURN_TRUE();
  } else {
    TRI_V8_RETURN_FALSE();
  }
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Chmod
////////////////////////////////////////////////////////////////////////////////

static void JS_ChMod(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("chmod(<path>, <mode>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  std::string const modeStr = TRI_ObjectToString(isolate, args[1]);
  size_t const length = modeStr.length();

  if (length == 0 || length > 5) {
    TRI_V8_THROW_TYPE_ERROR(
        "<mode> must be a string with up to 4 octal digits in it plus a "
        "leading zero.");
  }

  long mode = 0;
  for (uint32_t i = 0; i < length; ++i) {
    if (modeStr[i] < '0' || modeStr[i] > '9') {
      TRI_V8_THROW_TYPE_ERROR(
          "<mode> must be a string with up to 4 octal digits in it plus a "
          "leading zero.");
    }
    char buf[2];
    buf[0] = modeStr[i];
    buf[1] = '\0';
    uint8_t digit = (uint8_t)atoi(buf);
    if (digit >= 8) {
      TRI_V8_THROW_TYPE_ERROR(
          "<mode> must be a string with up to 4 octal digits in it plus a "
          "leading zero.");
    }
    mode = mode | digit << ((length - i - 1) * 3);
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + *name);
  }

  std::string err;
  auto rc = TRI_ChMod(*name, mode, err);

  if (rc != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(rc, err);
  }

  TRI_V8_RETURN_TRUE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Size
////////////////////////////////////////////////////////////////////////////////

static void JS_SizeFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("size(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  if (!TRI_ExistsFile(*name) || TRI_IsDirectory(*name)) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FILE_NOT_FOUND);
  }

  int64_t size = TRI_SizeFile(*name);

  if (size < 0) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FILE_NOT_FOUND);
  }

  TRI_V8_RETURN(v8::Number::New(isolate, (double)size));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads in a line from stdin
////////////////////////////////////////////////////////////////////////////////

static void JS_Getline(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

#ifdef _WIN32
  std::wstring wline;
  _setmode(_fileno(stdin), _O_U16TEXT);
  std::getline(std::wcin, wline);

  TRI_V8_RETURN_STD_WSTRING(wline);
#else
  std::string line;
  getline(std::cin, line);

  TRI_V8_RETURN_STD_STRING(line);
#endif
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_GetTempPath
////////////////////////////////////////////////////////////////////////////////

static void JS_GetTempPath(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("getTempPath()");
  }

  std::string path = TRI_GetTempPath();
  v8::Handle<v8::Value> result = TRI_V8_STD_STRING(isolate, path);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, path, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + path);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_GetTempFile
////////////////////////////////////////////////////////////////////////////////

static void JS_GetTempFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("getTempFile(<directory>, <createFile>)");
  }

  char const* p = nullptr;
  std::string path;
  if (args.Length() > 0) {
    path = TRI_ObjectToString(isolate, args[0]);
    p = path.c_str();
  }

  bool create = false;
  if (args.Length() > 1) {
    create = TRI_ObjectToBoolean(isolate, args[1]);
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  std::string dir = FileUtils::buildFilename(TRI_GetTempPath(),path,"");

  if (create) {
    if (!v8security.isAllowedToAccessPath(isolate, dir, FSAccessType::WRITE)) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                     std::string("not allowed to read files in this path: ") + dir);
    }
  } else {
    if (!v8security.isAllowedToAccessPath(isolate, dir, FSAccessType::READ)) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                     std::string("not allowed to read files in this path: ") + dir);
    }
  }


  std::string result;
  long systemError;
  std::string errorMessage;
  if (TRI_GetTempName(p, result, create, systemError, errorMessage) != TRI_ERROR_NO_ERROR) {
    errorMessage = std::string("could not create temp file: ") + errorMessage;
    TRI_V8_THROW_EXCEPTION_INTERNAL(errorMessage);
  }

  // return result
  TRI_V8_RETURN_STD_STRING(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_IsDirectory
////////////////////////////////////////////////////////////////////////////////

static void JS_IsDirectory(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("isDirectory(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  // return result
  if (TRI_IsDirectory(*name)) {
    TRI_V8_RETURN_TRUE();
  } else {
    TRI_V8_RETURN_FALSE();
  }
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_IsFile
////////////////////////////////////////////////////////////////////////////////

static void JS_IsFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("isFile(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  // return result
  if (TRI_ExistsFile(*name) && !TRI_IsDirectory(*name)) {
    TRI_V8_RETURN_TRUE();
  } else {
    TRI_V8_RETURN_FALSE();
  }
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_MakeAbsolute
////////////////////////////////////////////////////////////////////////////////

static void JS_MakeAbsolute(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("makeAbsolute(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  FileResultString cwd = FileUtils::currentDirectory();

  if (!cwd.ok()) {
    errno = cwd.sysErrorNumber();
    auto res = TRI_set_errno(TRI_ERROR_SYS_ERROR);
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        res, StringUtils::concatT("cannot get current working directory: ",
                                  cwd.errorMessage()));
  }

  std::string abs = TRI_GetAbsolutePath(std::string(*name, name.length()), cwd.result());

  v8::Handle<v8::String> res;

  if (!abs.empty()) {
    res = TRI_V8_STD_STRING(isolate, abs);
  } else {
    res = TRI_V8_STD_STRING(isolate, cwd.result());
  }

  // return result
  TRI_V8_RETURN(res);
  TRI_V8_TRY_CATCH_END
}


////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_List
////////////////////////////////////////////////////////////////////////////////

static void JS_List(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("listTree(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  // constructed listing
  v8::Handle<v8::Array> result = v8::Array::New(isolate);
  std::vector<std::string> list = TRI_FilesDirectory(*name);

  uint32_t j = 0;
  auto context = TRI_IGETC;

  for (auto const& f : list) {
    result->Set(context, j++, TRI_V8_STD_STRING(isolate, f)).FromMaybe(false);
  }

  // return result
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_ListTree
////////////////////////////////////////////////////////////////////////////////

static void JS_ListTree(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("listTree(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  // constructed listing
  v8::Handle<v8::Array> result = v8::Array::New(isolate);
  std::vector<std::string> files(TRI_FullTreeDirectory(*name));
  auto context = TRI_IGETC;
  uint32_t i = 0;
  for (auto const& it : files) {
    result->Set(context, i++, TRI_V8_STD_STRING(isolate, it)).FromMaybe(false);
  }

  // return result
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_MakeDirectory
////////////////////////////////////////////////////////////////////////////////

static void JS_MakeDirectory(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // 2nd argument (permissions) are ignored for now

  // extract arguments
  if (args.Length() != 1 && args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("makeDirectory(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + *name);
  }

  long systemError = 0;
  std::string systemErrorStr;
  auto res = TRI_CreateDirectory(*name, systemError, systemErrorStr);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, systemErrorStr);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_MakeDirectoryRecursive
////////////////////////////////////////////////////////////////////////////////

static void JS_MakeDirectoryRecursive(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // 2nd argument (permissions) are ignored for now

  // extract arguments
  if (args.Length() != 1 && args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("makeDirectoryRecursive(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + *name);
  }

  long systemError = 0;
  std::string systemErrorStr;
  auto res = TRI_CreateRecursiveDirectory(*name, systemError, systemErrorStr);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, systemErrorStr);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Unzip
////////////////////////////////////////////////////////////////////////////////

static void JS_UnzipFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "unzipFile(<filename>, <outPath>, <skipPaths>, <overwrite>, "
        "<password>)");
  }

  std::string const filename = TRI_ObjectToString(isolate, args[0]);
  std::string const outPath = TRI_ObjectToString(isolate, args[1]);

  bool skipPaths = false;
  if (args.Length() > 2) {
    skipPaths = TRI_ObjectToBoolean(isolate, args[2]);
  }

  bool overwrite = false;
  if (args.Length() > 3) {
    overwrite = TRI_ObjectToBoolean(isolate, args[3]);
  }

  char const* p = nullptr;
  std::string password;
  if (args.Length() > 4) {
    password = TRI_ObjectToString(isolate, args[4]);
    p = password.c_str();
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, filename, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + filename);
  }

  if (!v8security.isAllowedToAccessPath(isolate, outPath, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + outPath);
  }

  std::string errMsg;
  auto res = TRI_UnzipFile(filename.c_str(), outPath.c_str(), skipPaths, overwrite, p, errMsg);

  if (res == TRI_ERROR_NO_ERROR) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_THROW_EXCEPTION_MESSAGE(res, errMsg);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Zip
////////////////////////////////////////////////////////////////////////////////

static void JS_ZipFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() < 3 || args.Length() > 4) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "zipFile(<filename>, <chdir>, <files>, <password>)");
  }

  std::string const filename = TRI_ObjectToString(isolate, args[0]);
  std::string const dir = TRI_ObjectToString(isolate, args[1]);

  if (!args[2]->IsArray()) {
    TRI_V8_THROW_EXCEPTION_USAGE("<files> must be a list");
  }

  v8::Handle<v8::Array> files = v8::Handle<v8::Array>::Cast(args[2]);

  auto res = TRI_ERROR_NO_ERROR;
  std::vector<std::string> filenames;

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, filename, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
                    TRI_ERROR_FORBIDDEN, std::string("not allowed to read files in this path: ") + filename);
  }
  if (!v8security.isAllowedToAccessPath(isolate, dir, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
                    TRI_ERROR_FORBIDDEN, std::string("not allowed to read files in this path: ") + dir);
  }
  auto context = TRI_IGETC;
  for (uint32_t i = 0; i < files->Length(); ++i) {
    v8::Handle<v8::Value> file = files->Get(context, i).FromMaybe(v8::Handle<v8::Value>());
    if (file->IsString()) {
      if (!v8security.isAllowedToAccessPath(isolate, filename, FSAccessType::READ)) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_FORBIDDEN, std::string("not allowed to read files in this path: ") + filename);
      }
      filenames.emplace_back(TRI_ObjectToString(isolate, file));
    } else {
      res = TRI_ERROR_BAD_PARAMETER;
      break;
    }
  }

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "zipFile(<filename>, <chdir>, <files>, <password>)");
  }

  char const* p = nullptr;
  std::string password;
  if (args.Length() == 4) {
    password = TRI_ObjectToString(isolate, args[3]);
    p = password.c_str();
  }

  if (!v8security.isAllowedToAccessPath(isolate, filename, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + filename);
  }

  res = TRI_ZipFile(filename.c_str(), dir.c_str(), filenames, p);

  if (res == TRI_ERROR_NO_ERROR) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_THROW_EXCEPTION(res);
  TRI_V8_TRY_CATCH_END
}

static void JS_Adler32(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("adler32(<filename>, <useSigned>)");
  }

  bool useSigned = false;  // default is unsigned
  if (args.Length() > 1) {
    useSigned = TRI_ObjectToBoolean(isolate, args[1]);
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  uint32_t chksum = 0;
  auto res = TRI_Adler32(*name, chksum);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  v8::Handle<v8::Value> result;

  if (useSigned) {
    result = v8::Number::New(isolate, static_cast<int32_t>(chksum));
  } else {
    result = v8::Number::New(isolate, chksum);
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads a file and executes it
///
/// @FUN{internal.load(*filename*)}
///
/// Reads in a files and executes the contents in the current context.
////////////////////////////////////////////////////////////////////////////////

static void JS_Load(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("load(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  size_t length;
  char* content = TRI_SlurpFile(*name, &length);

  if (content == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), "cannot read file");
  }

  v8::Handle<v8::Value> filename = args[0];

  // save state of __dirname and __filename
  v8::Handle<v8::Object> current = isolate->GetCurrentContext()->Global();
  auto oldFilename = TRI_GetProperty(context, isolate, current, "__filename");
  current->Set(context, TRI_V8_ASCII_STRING(isolate, "__filename"), filename).FromMaybe(false);

  auto oldDirname = TRI_GetProperty(context, isolate, current, "__dirname");
  auto dirname = TRI_Dirname(TRI_ObjectToString(isolate, filename));
  current->Set(context,
               TRI_V8_ASCII_STRING(isolate, "__dirname"),
               TRI_V8_STD_STRING(isolate, dirname)).FromMaybe(false);

  v8::Handle<v8::Value> result;
  {
    v8::TryCatch tryCatch(isolate);

    result = TRI_ExecuteJavaScriptString(isolate, isolate->GetCurrentContext(),
                                         TRI_V8_PAIR_STRING(isolate, content, length),
                                         TRI_ObjectToString(context, filename), false);

    TRI_FreeString(content);

    // restore old values for __dirname and __filename
    if (oldFilename.IsEmpty() || oldFilename->IsUndefined()) {
      TRI_DeleteProperty(context, isolate, current, "__filename");
    } else {
      current->Set(context, TRI_V8_ASCII_STRING(isolate, "__filename"), oldFilename).FromMaybe(false);
    }
    if (oldDirname.IsEmpty() || oldDirname->IsUndefined()) {
      TRI_DeleteProperty(context, isolate, current, "__dirname");
    } else {
      current->Set(context, TRI_V8_ASCII_STRING(isolate, "__dirname"), oldDirname).FromMaybe(false);
    }
    if (result.IsEmpty()) {
      if (tryCatch.CanContinue()) {
        TRI_V8_LOG_THROW_EXCEPTION(tryCatch);
      } else {
        tryCatch.ReThrow();
        v8g->_canceled = true;
        TRI_V8_RETURN_UNDEFINED();
      }
    }
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief logs a message
///
/// @FUN{internal.log(@FA{level}, @FA{message})}
///
/// Logs the @FA{message} at the given log @FA{level}.
///
/// Valid log-level are:
///
/// - fatal
/// - error
/// - warning
/// - info
/// - debug
/// - trace
////////////////////////////////////////////////////////////////////////////////

static void JS_Log(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("log(<level>, <message>)");
  }

  TRI_Utf8ValueNFC level(isolate, args[0]);

  if (*level == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<level> must be a string");
  }

  std::vector<std::string> splitted = StringUtils::split(*level, '=');

  std::string ls = "info";
  std::string ts = "";

  if (splitted.size() == 1) {
    ts = splitted[0];
  } else if (splitted.size() >= 2) {
    ts = splitted[0];
    ls = splitted[1];
  }

  StringUtils::tolowerInPlace(ls);
  StringUtils::tolowerInPlace(ts);

  std::string prefix;
  if (ls == "fatal") {
    prefix = "FATAL! ";
  } else if (ls != "error" && ls != "warning" && ls != "warn" && ls != "info" && ls != "debug" && ls != "trace") {
    // invalid log level
    prefix = ls + "!";
  }

  LogTopic const* topicPtr = ts.empty() ? nullptr : LogTopic::lookup(ts);
  LogTopic const& topic = (topicPtr != nullptr) ? *topicPtr : Logger::FIXME;

  auto logMessage = [&](auto const& message) {
    if (ls == "fatal") {
      LOG_TOPIC("ecbc6", FATAL, topic) << prefix << message;
    } else if (ls == "error") {
      LOG_TOPIC("24213", ERR, topic) << prefix << message;
    } else if (ls == "warning" || ls == "warn") {
      LOG_TOPIC("514da", WARN, topic) << prefix << message;
    } else if (ls == "info") {
      LOG_TOPIC("99d80", INFO, topic) << prefix << message;
    } else if (ls == "debug") {
      LOG_TOPIC("f3533", DEBUG, topic) << prefix << message;
    } else if (ls == "trace") {
      LOG_TOPIC("74c21", TRACE, topic) << prefix << message;
    } else {
      LOG_TOPIC("6b817", WARN, topic) << prefix << message;
    }
  };

  auto context = TRI_IGETC;

  if (args[1]->IsArray()) {
    auto loglines = v8::Handle<v8::Array>::Cast(args[1]);

    for (uint32_t i = 0; i < loglines->Length(); ++i) {
      v8::Handle<v8::Value> line = loglines->Get(context, i).FromMaybe(v8::Handle<v8::Value>());
      TRI_Utf8ValueNFC message(isolate, line);
      if (line->IsString()) {
        logMessage(*message);
      }
    }
  } else {
    TRI_Utf8ValueNFC message(isolate, args[1]);

    if (*message == nullptr) {
      TRI_V8_THROW_TYPE_ERROR("<message> must be a string or an array");
    }

    logMessage(*message);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief gets or sets the log-level
///
/// @FUN{internal.logLevel()}
///
/// Returns the current global log-level as string.
///
/// @FUN{internal.logLevel(true)}
///
/// Returns all topic log-level as vector of strings.
///
/// @FUN{internal.logLevel(@FA{level})}
///
/// Changes the current log-level. Valid log-level are:
///
/// - fatal
/// - error
/// - warning
/// - info
/// - debug
/// - trace
////////////////////////////////////////////////////////////////////////////////

static void JS_LogLevel(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (v8security.isInternalModuleHardened(isolate)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "not allowed to change logLevel");
  }

  auto context = TRI_IGETC;
  if (1 <= args.Length()) {
    if (args[0]->IsBoolean()) {
      auto levels = Logger::logLevelTopics();
      size_t n = levels.size();
      v8::Handle<v8::Array> object = v8::Array::New(isolate, static_cast<int>(n));

      uint32_t pos = 0;

      for (auto level : levels) {
        std::string output = level.first + "=" + Logger::translateLogLevel(level.second);
        v8::Handle<v8::String> val = TRI_V8_STD_STRING(isolate, output);

        object->Set(context, pos++, val).FromMaybe(false);
      }

      TRI_V8_RETURN(object);
    } else {
      TRI_Utf8ValueNFC str(isolate, args[0]);
      Logger::setLogLevel(*str);
      TRI_V8_RETURN_UNDEFINED();
    }
  } else {
    auto level = Logger::translateLogLevel(Logger::logLevel());
    TRI_V8_RETURN_STD_STRING(level);
  }

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the md5 sum of a text
///
/// @FUN{internal.md5(@FA{text})}
///
/// Computes an md5 for the @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Md5(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("md5(<text>)");
  }

  v8::String::Utf8Value str(isolate, args[0]);

  if (*str == nullptr) {
    TRI_V8_RETURN_UNDEFINED();
  }

  // create md5
  char hash[17];
  char* p = &hash[0];
  size_t length;

  SslInterface::sslMD5(*str, str.length(), p, length);

  // as hex
  char hex[33];
  p = &hex[0];

  SslInterface::sslHEX(hash, 16, p, length);

  // and return
  v8::Handle<v8::String> hashStr = TRI_V8_PAIR_STRING(isolate, hex, 32);

  TRI_V8_RETURN(hashStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates random numbers
///
/// @FUN{internal.genRandomNumbers(@FA{length})}
///
/// Generates a string of a given @FA{length} containing numbers.
////////////////////////////////////////////////////////////////////////////////

static void JS_RandomNumbers(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1 || !args[0]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE("genRandomNumbers(<length>)");
  }

  int length = (int)TRI_ObjectToInt64(isolate, args[0]);

  if (length <= 0 || length > 65536) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "<length> must be between 0 and 65536");
  }

  std::string str = JSNumGenerator.random(length);
  TRI_V8_RETURN_STD_STRING(str);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates random alpha-numbers
///
/// @FUN{internal.genRandomAlphaNumbers(@FA{length})}
///
/// Generates a string of a given @FA{length} containing numbers and characters.
////////////////////////////////////////////////////////////////////////////////

static void JS_RandomAlphaNum(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1 || !args[0]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE("genRandomAlphaNumbers(<length>)");
  }

  int length = (int)TRI_ObjectToInt64(isolate, args[0]);
  if (length <= 0 || length > 65536) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "<length> must be between 0 and 65536");
  }

  std::string&& str = JSAlphaNumGenerator.random(length);
  TRI_V8_RETURN_STD_STRING(str);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a salt
///
/// @FUN{internal.genRandomSalt(@FA{length})}
///
/// Generates a string of a given @FA{length} containing ASCII characters.
////////////////////////////////////////////////////////////////////////////////

static void JS_RandomSalt(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1 || !args[0]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE("genRandomSalt(<length>)");
  }

  int length = (int)TRI_ObjectToInt64(isolate, args[0]);
  if (length <= 0 || length > 65536) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "<length> must be between 0 and 65536");
  }

  std::string str = JSSaltGenerator.random(length);
  TRI_V8_RETURN_STD_STRING(str);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a nonce
///
/// @FUN{internal.createNonce()}
///
/// Generates a base64 encoded nonce string. (length of the string is 16)
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateNonce(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("createNonce()");
  }

  std::string str = Nonce::createNonce();

  TRI_V8_RETURN_STD_STRING(str);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks a base64 encoded nonce
///
/// @FUN{internal.checkAndMarkNonce(@FA{nonce})}
///
/// Checks and marks a @FA{nonce}
////////////////////////////////////////////////////////////////////////////////

static void JS_MarkNonce(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("checkAndMarkNonce(<nonce>)");
  }

  TRI_Utf8ValueNFC base64u(isolate, args[0]);

  if (*base64u == nullptr || base64u.length() != 16) {
    TRI_V8_THROW_TYPE_ERROR("expecting 16-Byte base64url-encoded nonce");
  }

  std::string raw = StringUtils::decodeBase64U(*base64u);

  if (raw.size() != 12) {
    TRI_V8_THROW_TYPE_ERROR("expecting 12-Byte nonce");
  }

  if (Nonce::checkAndMark(raw)) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_MTime
////////////////////////////////////////////////////////////////////////////////

static void JS_MTime(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract two arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("mtime(<filename>)");
  }

  std::string filename = TRI_ObjectToString(isolate, args[0]);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, filename, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + filename);
  }

  int64_t mtime = 0;
  auto res = TRI_MTimeFile(filename.c_str(), &mtime);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_V8_RETURN(v8::Number::New(isolate, static_cast<double>(mtime)));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_MoveFile
////////////////////////////////////////////////////////////////////////////////

static void JS_MoveFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract two arguments
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("move(<source>, <destination>)");
  }

  std::string source = TRI_ObjectToString(isolate, args[0]);
  std::string destination = TRI_ObjectToString(isolate, args[1]);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, source, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + source);
  }

  if (!v8security.isAllowedToAccessPath(isolate, destination, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + destination);
  }

  bool const sourceIsDirectory = TRI_IsDirectory(source.c_str());
  bool const destinationIsDirectory = TRI_IsDirectory(destination.c_str());

  if (sourceIsDirectory && destinationIsDirectory) {
    // source is a directory, destination is a directory. this is unsupported
    TRI_V8_THROW_EXCEPTION_PARAMETER(
        "cannot move source directory into destination directory");
  }

  if (TRI_IsRegularFile(source.c_str()) && destinationIsDirectory) {
    // source is a file, destination is a directory. this is unsupported
    TRI_V8_THROW_EXCEPTION_PARAMETER(
        "cannot move source file into destination directory");
  }

  std::string systemErrorStr;
  long errorNo;

  auto res = TRI_RenameFile(source.c_str(), destination.c_str(), &errorNo, &systemErrorStr);

  if (res != TRI_ERROR_NO_ERROR) {
    std::string errMsg = "cannot move file [" + source + "] to [" + destination +
                         " ] : " + std::to_string(errorNo) + ": " + systemErrorStr;
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, errMsg);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_CopyDirectoryRecursive
////////////////////////////////////////////////////////////////////////////////

static void JS_CopyRecursive(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract two arguments
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("copyRecursive(<source>, <destination>)");
  }

  std::string source = TRI_ObjectToString(isolate, args[0]);
  std::string destination = TRI_ObjectToString(isolate, args[1]);

  bool const sourceIsDirectory = TRI_IsDirectory(source.c_str());
  bool const destinationIsDirectory = TRI_IsDirectory(destination.c_str());

  if (sourceIsDirectory && destinationIsDirectory) {
    // source is a directory, destination is a directory. this is unsupported
    TRI_V8_THROW_EXCEPTION_PARAMETER(
        "cannot copy source directory into destination directory");
  }

  if (TRI_IsRegularFile(source.c_str()) && destinationIsDirectory) {
    // source is a file, destination is a directory. this is unsupported
    TRI_V8_THROW_EXCEPTION_PARAMETER(
        "cannot copy source file into destination directory");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, source, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + source);
  }

  if (!v8security.isAllowedToAccessPath(isolate, destination, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + destination);
  }

  std::string systemErrorStr;
  long errorNo;
  auto res = TRI_CreateRecursiveDirectory(destination.c_str(), errorNo, systemErrorStr);

  if (res != TRI_ERROR_NO_ERROR) {
    std::string errMsg = "cannot copy file [" + source + "] to [" +
                         destination + " ] : " + std::to_string(errorNo) +
                         " - Unable to create target directory: " + systemErrorStr;
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_SYS_ERROR, errMsg);
  }

  std::function<bool(std::string const&)> const passAllFilter =
      [](std::string const&) { return false; };

  if (!FileUtils::copyRecursive(source, destination, passAllFilter, systemErrorStr)) {
    std::string errMsg = "cannot copy directory [" + source + "] to [" + destination +
                         " ] : " + std::to_string(errorNo) + ": " + systemErrorStr;
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, errMsg);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_CopyFile
////////////////////////////////////////////////////////////////////////////////

static void JS_CopyFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract two arguments
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("copyFile(<source>, <destination>)");
  }

  std::string source = TRI_ObjectToString(isolate, args[0]);
  std::string destination = TRI_ObjectToString(isolate, args[1]);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, source, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + source);
  }

  if (!v8security.isAllowedToAccessPath(isolate, destination, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + destination);
  }

  bool const destinationIsDirectory = TRI_IsDirectory(destination.c_str());

  if (!TRI_IsRegularFile(source.c_str())) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("can only copy regular files.");
  }

  std::string systemErrorStr;

  if (destinationIsDirectory) {
    char const* file = strrchr(source.c_str(), TRI_DIR_SEPARATOR_CHAR);
    if (file == nullptr) {
      if (!destination.empty() && destination.back() != TRI_DIR_SEPARATOR_CHAR) {
        destination += TRI_DIR_SEPARATOR_CHAR;
      }
      destination += source;
    } else {
      destination += file;
    }
  }

  if (!TRI_CopyFile(source, destination, systemErrorStr)) {
    std::string errMsg = "cannot copy file [" + source + "] to [" +
                         destination + " ] : " + systemErrorStr;
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, errMsg);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_CopyFile
////////////////////////////////////////////////////////////////////////////////

static void JS_LinkFile(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract two arguments
  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("linkFile(<target>, <linkpath>)");
  }

  std::string target = TRI_ObjectToString(isolate, args[0]);
  std::string linkpath = TRI_ObjectToString(isolate, args[1]);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, target, FSAccessType::READ)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + target);
  }

  if (!v8security.isAllowedToAccessPath(isolate, linkpath, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + linkpath);
  }

  std::string systemErrorStr;

  if (!TRI_CreateSymbolicLink(target, linkpath, systemErrorStr)) {
    std::string errMsg = "cannot create a symlink from [" + target + "] to [" +
                         linkpath + " ] : " + systemErrorStr;
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, errMsg);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads from stdin
////////////////////////////////////////////////////////////////////////////////

static void JS_PollStdin(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() > 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("pollStdin()");
  }

  bool hasData = false;
#ifdef _WIN32
  auto hin = ::GetStdHandle(STD_INPUT_HANDLE);
  if (GetFileType(hin) == FILE_TYPE_PIPE) {
    DWORD numBytes = 0;
    if (PeekNamedPipe(hin, nullptr, 0, nullptr, &numBytes, nullptr)) {
      hasData = numBytes > 0;
    }
  } else {
    hasData = _kbhit() != 0;
  }
#else
  struct timeval tv;
  fd_set fds;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
  hasData = FD_ISSET(STDIN_FILENO, &fds);
#endif

  if (hasData) {
    char c[1024] = {0};
    TRI_read_return_t n = TRI_READ(STDIN_FILENO, c, sizeof(c) - 1);
    TRI_V8_RETURN_STD_STRING(std::string(c, n));
  }
  TRI_V8_RETURN(v8::Integer::New(isolate, 0));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief outputs the arguments
///
/// @FUN{internal.output(@FA{string1}, @FA{string2}, @FA{string3}, ...)}
///
/// Outputs the arguments to standard output.
///
/// @verbinclude fluent39
////////////////////////////////////////////////////////////////////////////////

static void JS_Output(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  for (int i = 0; i < args.Length(); i++) {
    // extract the next argument
    v8::Handle<v8::Value> val = args[i];

    // convert it into a string
    // v8::String::Utf8Value utf8(val);
    TRI_Utf8ValueNFC utf8(isolate, val);

    if (*utf8 == nullptr) {
      continue;
    }

    // print the argument
    char const* ptr = *utf8;
    size_t len = utf8.length();

    while (0 < len) {
      ssize_t n = TRI_WRITE(STDOUT_FILENO, ptr, static_cast<TRI_write_t>(len));

      if (n < 0) {
        break;
      }

      len -= n;
      ptr += n;
    }
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the current process information
/// `internal.processStatistics()`
///
/// Returns information about the current process:
///
/// - minorPageFaults: The number of minor faults the process has made
///   which have not required loading a memory page from disk.
///
/// - majorPageFaults: The number of major faults the process has made
///   which have required loading a memory page from disk.
///
/// - userTime: Amount of time that this process has been scheduled in
///   user mode, measured in clock ticks.
///
/// - systemTime: Amount of time that this process has been scheduled
///   in kernel mode, measured in clock ticks.
///
/// - numberOfThreads: Number of threads in this process.
///
/// - residentSize: Resident Set Size: total size of the number of pages
///   the process has in real memory.  This is just the pages which count
///   toward text, data, or stack space.  This does not include pages which
///   have not been demand-loaded in, or which are swapped out.
///
///   The resident set size is reported in bytes.
///
/// - residentSizePercent: resident size as percent of the total physical
///   memory size.
///
/// - virtualSize: Virtual memory size in bytes.
///
/// @EXAMPLE_ARANGOSH_OUTPUT{internalStatistics}
///   require("internal").processStat();
/// @END_EXAMPLE_ARANGOSH_OUTPUT
////////////////////////////////////////////////////////////////////////////////

static void ProcessStatisticsToV8(v8::FunctionCallbackInfo<v8::Value> const& args, ProcessInfo const& info) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  double rss = (double)info._residentSize;
  double rssp = 0;

  if (PhysicalMemory::getValue() != 0) {
    rssp = rss / PhysicalMemory::getValue();
  }

  auto context = TRI_IGETC;
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "minorPageFaults"),
              v8::Number::New(isolate, (double)info._minorPageFaults)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "majorPageFaults"),
              v8::Number::New(isolate, (double)info._majorPageFaults)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "userTime"),
              v8::Number::New(isolate, (double)info._userTime / (double)info._scClkTck)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "systemTime"),
              v8::Number::New(isolate, (double)info._systemTime / (double)info._scClkTck)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "numberOfThreads"),
              v8::Number::New(isolate, (double)info._numberThreads)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "residentSize"), v8::Number::New(isolate, rss)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "residentSizePercent"),
              v8::Number::New(isolate, rssp)).FromMaybe(false);
  result->Set(context, TRI_V8_ASCII_STRING(isolate, "virtualSize"),
              v8::Number::New(isolate, (double)info._virtualSize)).FromMaybe(false);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

static void JS_ProcessStatisticsSelf(v8::FunctionCallbackInfo<v8::Value> const& args) {
  ProcessStatisticsToV8(args, TRI_ProcessInfoSelf());
}

static void JS_ProcessStatisticsExternal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "statisticsExternal(<external-identifier>)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (v8security.isInternalModuleHardened(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  ExternalId pid;

  pid._pid = static_cast<TRI_pid_t>(TRI_ObjectToUInt64(isolate, args[0], true));

  ProcessStatisticsToV8(args, TRI_ProcessInfo(pid._pid));
  TRI_V8_TRY_CATCH_END
}

static void JS_GetPid(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (v8security.isInternalModuleHardened(isolate)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   "not allowed to provide this information");
  }

  // check arguments
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("getPid()");
  }
  TRI_pid_t pid = Thread::currentProcessId();
  TRI_V8_RETURN_INTEGER(pid);

  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generates a random number using OpenSSL
///
/// @FUN{internal.rand()}
///
/// Generates a random number
////////////////////////////////////////////////////////////////////////////////

static void JS_Rand(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // check arguments
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("rand()");
  }

  int iterations = 0;
  while (iterations++ < 5) {
    int32_t value;
    int result = SslInterface::sslRand(&value);

    if (result != 0) {
      // error
      break;
    }

    // no error, now check what random number was produced

    if (value != 0) {
      // a number != 0 was produced. that is sufficient
      TRI_V8_RETURN(v8::Number::New(isolate, value));
    }

    // we don't want to return 0 as the result, so we try again
  }

  // we failed to produce a valid random number
  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Read
////////////////////////////////////////////////////////////////////////////////

static void JS_Read(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("read(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  size_t length;
  char* content = TRI_SlurpFile(*name, &length);

  if (content == nullptr) {
    auto msg = std::string{TRI_last_error()};
    msg += ": while reading ";
    msg += *name;
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), msg);
  }

  auto result = TRI_V8_PAIR_STRING(isolate, content, length);

  TRI_FreeString(content);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Read
////////////////////////////////////////////////////////////////////////////////

static void JS_ReadPipe(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("readPipe(<external-identifier>)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  TRI_pid_t pid = static_cast<TRI_pid_t>(TRI_ObjectToInt64(isolate, args[0]));
  ExternalProcess const* proc = TRI_LookupSpawnedProcess(pid);

  if (proc == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
      TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
      "didn't find the process identified by <external-identifier> to read the pipe from.");
  }

  char content[1024];
  size_t length = sizeof(content) - 1;
  auto read_len = TRI_ReadPipe(proc, content, length);

  auto result = TRI_V8_PAIR_STRING(isolate, content, read_len);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Read
////////////////////////////////////////////////////////////////////////////////

static void JS_WritePipe(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("writePipe(<external-identifier>, string)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  TRI_pid_t pid = static_cast<TRI_pid_t>(TRI_ObjectToInt64(isolate, args[0]));
  ExternalProcess const* proc = TRI_LookupSpawnedProcess(pid);

  if (proc == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
      TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
      "didn't find the process identified by <external-identifier> to write the pipe into.");
  }

  std::string writeBuffer = TRI_ObjectToString(isolate, args[1]);
  if (TRI_WritePipe(proc, writeBuffer.c_str(), writeBuffer.length())) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

static void JS_ClosePipe(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("closePipe(<external-identifier>, bool read)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  TRI_pid_t pid = static_cast<TRI_pid_t>(TRI_ObjectToInt64(isolate, args[0]));
  ExternalProcess* proc = TRI_LookupSpawnedProcess(pid);

  if (proc == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
      TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
      "didn't find the process identified by <external-identifier> to close the pipe for.");
  }

  bool read = TRI_ObjectToBoolean(isolate, args[1]);
  TRI_ClosePipe(proc, read);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_ReadBuffer
////////////////////////////////////////////////////////////////////////////////

static void JS_ReadBuffer(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("readBuffer(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  size_t length;
  char* content = TRI_SlurpFile(*name, &length);

  if (content == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  V8Buffer* buffer = V8Buffer::New(isolate, content, length);

  TRI_FreeString(content);
  TRI_V8_RETURN(v8::Local<v8::Object>::New(isolate, buffer->_handle));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_ReadGzip
////////////////////////////////////////////////////////////////////////////////

static void JS_ReadGzip(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("readGzip(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  size_t length;
  char* content = TRI_SlurpGzipFile(*name, &length);

  if (content == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  auto result = TRI_V8_PAIR_STRING(isolate, content, length);

  TRI_FreeString(content);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END

}

#ifdef USE_ENTERPRISE
////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_ReadGzip
////////////////////////////////////////////////////////////////////////////////

static void JS_ReadDecrypt(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("readDecrypt(<filename>,<keyfilename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_Utf8ValueNFC keyfile(isolate, args[1]);

  if (*keyfile == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<keyfilename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  if (!v8security.isAllowedToAccessPath(isolate, *keyfile, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *keyfile);
  }

  size_t length;
  auto& encryptionFeature = v8g->_server.getFeature<EncryptionFeature>();
  char* content = TRI_SlurpDecryptFile(encryptionFeature, *name, *keyfile, &length);

  if (content == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  auto result = TRI_V8_PAIR_STRING(isolate, content, length);

  TRI_FreeString(content);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END

}
#endif

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Read64
////////////////////////////////////////////////////////////////////////////////

static void JS_Read64(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("read(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a UTF-8 string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  std::string base64;

  try {
    std::string content = FileUtils::slurp(*name);
    base64 = StringUtils::encodeBase64(content);
  } catch (...) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), TRI_last_error());
  }

  TRI_V8_RETURN_STD_STRING(base64);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief append to a file
////////////////////////////////////////////////////////////////////////////////

static void JS_Append(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("append(<filename>, <content>)");
  }

#if _WIN32  // the wintendo needs utf16 filenames
  v8::String::Value str(isolate, args[0]);
  std::wstring name{reinterpret_cast<wchar_t*>(*str), static_cast<size_t>(str.length())};
  TRI_Utf8ValueNFC utf8Str(isolate, args[0]);
  std::string utf8Name(*utf8Str, utf8Str.length());
#else
  TRI_Utf8ValueNFC str(isolate, args[0]);
  std::string name(*str, str.length());
  std::string const& utf8Name = name;
#endif

  if (name.empty()) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a non-empty string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, utf8Name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + utf8Name);
  }

  std::ofstream file;

  if (args[1]->IsObject() && V8Buffer::hasInstance(isolate, args[1])) {
    // content is a buffer
    char const* data = V8Buffer::data(isolate, args[1].As<v8::Object>());
    size_t size = V8Buffer::length(isolate, args[1].As<v8::Object>());

    if (data == nullptr) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "invalid <content> buffer value");
    }

    file.open(name, std::ios::out | std::ios::binary | std::ios::app);

    if (file.is_open()) {
      file.write(data, size);
      file.close();
      TRI_V8_RETURN_TRUE();
    }
  } else {
    TRI_Utf8ValueNFC content(isolate, args[1]);

    if (*content == nullptr) {
      TRI_V8_THROW_TYPE_ERROR("<content> must be a string");
    }

    file.open(name, std::ios::out | std::ios::binary | std::ios::app);

    if (file.is_open()) {
      file.write(*content, content.length());
      file.close();
      TRI_V8_RETURN_TRUE();
    }
  }

  TRI_V8_THROW_EXCEPTION_SYS("cannot write file");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write to a file
////////////////////////////////////////////////////////////////////////////////

static void JS_Write(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("write(<filename>, <content>)");
  }
#if _WIN32  // the wintendo needs utf16 filenames
  v8::String::Value str(isolate, args[0]);
  std::wstring name{reinterpret_cast<wchar_t*>(*str), static_cast<size_t>(str.length())};
  TRI_Utf8ValueNFC utf8Str(isolate, args[0]);
  std::string utf8Name(*utf8Str, utf8Str.length());
#else
  TRI_Utf8ValueNFC str(isolate, args[0]);
  std::string name(*str, str.length());
  std::string const& utf8Name = name;
#endif

  if (name.length() == 0) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, utf8Name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + utf8Name);
  }

  bool flush = false;
  if (args.Length() > 2) {
    flush = TRI_ObjectToBoolean(isolate, args[2]);
  }

  if (args[1]->IsObject() && V8Buffer::hasInstance(isolate, args[1])) {
    // content is a buffer
    char const* data = V8Buffer::data(isolate, args[1].As<v8::Object>());
    size_t size = V8Buffer::length(isolate, args[1].As<v8::Object>());

    if (data == nullptr) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "invalid <content> buffer value");
    }

    std::fstream file;

    errno = 0;
    // disable exceptions in the stream object:
    file.exceptions(std::ifstream::goodbit);
    file.open(name, std::ios::out | std::ios::binary);

    if (file.is_open() && file.good()) {
      file.write(data, size);
      if (file.good()) {
        if (flush) {
          file.flush();
          file.sync();
        }
        file.close();
        if (file.good()) {
          TRI_V8_RETURN_TRUE();
        }
      } else {
        file.close();
      }
    }
  } else {
    TRI_Utf8ValueNFC content(isolate, args[1]);

    if (*content == nullptr) {
      TRI_V8_THROW_TYPE_ERROR("<content> must be a string");
    }

    std::fstream file;

    errno = 0;
    // disable exceptions in the stream object:
    file.exceptions(std::ifstream::goodbit);
    file.open(name, std::ios::out | std::ios::binary);

    if (file.is_open() && file.good()) {
      file << *content;
      if (file.good()) {
        if (flush) {
          file.flush();
          file.sync();
        }
        file.close();
        if (file.good()) {
          TRI_V8_RETURN_TRUE();
        }
      } else {
        file.close();
      }
    }
  }

  TRI_V8_THROW_EXCEPTION_SYS("cannot write file");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_Remove
////////////////////////////////////////////////////////////////////////////////

static void JS_Remove(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("remove(<filename>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + *name);
  }

  auto res = TRI_UnlinkFile(*name);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, "cannot remove file");
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_RemoveDirectory
////////////////////////////////////////////////////////////////////////////////

static void JS_RemoveDirectory(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("removeDirectory(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + *name);
  }

  if (!TRI_IsDirectory(*name)) {
    std::string err =
        std::string("<path> must be a valid directory name (have '") + *name +
        "')";
    TRI_V8_THROW_EXCEPTION_PARAMETER(err);
  }

  auto res = TRI_RemoveEmptyDirectory(*name);

  if (res != TRI_ERROR_NO_ERROR) {
    std::string err = std::string("cannot remove directory: ") + *name + "'";
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, err);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock JS_RemoveDirectoryRecursive
////////////////////////////////////////////////////////////////////////////////

static void JS_RemoveRecursiveDirectory(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("removeDirectoryRecursive(<path>)");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<path> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::WRITE)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to modify files in this path: ") + *name);
  }

  if (!TRI_IsDirectory(*name)) {
    std::string err =
        std::string("<path> must be a valid directory name (have '") + *name +
        "')";
    TRI_V8_THROW_EXCEPTION_PARAMETER(err);
  }

  bool force = false;

  if (args.Length() > 1) {
    force = TRI_ObjectToBoolean(isolate, args[1]);
  }

  if (!force) {
    // check if we're inside the temp directory. force will override this check
    std::string tempPath = TRI_GetTempPath();

    if (tempPath.size() < 6) {
      TRI_V8_THROW_EXCEPTION_PARAMETER(
          "temporary directory name is too short. will not remove directory");
    }

    std::string const path(*name);

#ifdef _WIN32
    // windows paths are case-insensitive
    if (!TRI_CaseEqualString(path.c_str(), tempPath.c_str(), tempPath.size())) {
#else
    if (!TRI_EqualString(path.c_str(), tempPath.c_str(), tempPath.size())) {
#endif
      std::string errorMessage = std::string("directory to be removed [") +
                                 path + "] is outside of temporary path [" +
                                 tempPath + "]";
      TRI_V8_THROW_EXCEPTION_PARAMETER(errorMessage);
    }
  }

  auto res = TRI_RemoveDirectory(*name);

  if (res != TRI_ERROR_NO_ERROR) {
    std::string err = std::string("cannot remove directory: ") + *name + "'";
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, err);
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief formats the arguments
///
/// @FUN{internal.sprintf(@FA{format}, @FA{argument1}, ...)}
///
/// Formats the arguments according to the format string @FA{format}.
////////////////////////////////////////////////////////////////////////////////

static void JS_SPrintF(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  size_t len = args.Length();

  if (len == 0) {
    TRI_V8_RETURN_STRING("");
  }

  TRI_Utf8ValueNFC format(isolate, args[0]);

  if (*format == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<format> must be a UTF-8 string");
  }

  std::string result;

  size_t p = 1;

  for (char const* ptr = *format; *ptr; ++ptr) {
    if (*ptr == '%') {
      ++ptr;

      switch (*ptr) {
        case '%':
          result += '%';
          break;

        case 'd':
        case 'f':
        case 'i': {
          if (len <= p) {
            result += "%";
            result += *ptr;
          } else {
            bool e;
            double f = TRI_ObjectToDouble(isolate, args[(int)p], e);

            if (e) {
              result += "NaN";
            } else {
              char b[1024];

              if (*ptr == 'f') {
                snprintf(b, sizeof(b), "%f", f);
              } else {
                snprintf(b, sizeof(b), "%ld", (long)f);
              }

              result += b;
            }

            ++p;
          }

          break;
        }

        case 'o':
        case 's': {
          if (len <= p) {
            result += "%";
            result += *ptr;
          } else {
            TRI_Utf8ValueNFC text(isolate, args[(int)p]);

            if (*text == nullptr) {
              std::string msg = StringUtils::itoa(p) +
                                ".th argument must be a string in format '" +
                                *format + "'";
              TRI_V8_THROW_EXCEPTION_PARAMETER(msg.c_str());
            }

            ++p;

            result += *text;
          }

          break;
        }

        default:
          result += "%";
          result += *ptr;
          break;
      }
    } else {
      result += *ptr;
    }
  }

  for (size_t i = p; i < len; ++i) {
    TRI_Utf8ValueNFC text(isolate, args[(int)i]);

    if (*text == nullptr) {
      std::string msg = StringUtils::itoa(i) +
                        ".th argument must be a string in format '" + *format +
                        "'";
      TRI_V8_THROW_TYPE_ERROR(msg.c_str());
    }

    result += " ";
    result += *text;
  }

  TRI_V8_RETURN_STD_STRING(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the sha512 sum
///
/// @FUN{internal.sha512(@FA{text})}
///
/// Computes an sha512 for the @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Sha512(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("sha512(<text>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);

  // create sha512
  char* hash = nullptr;
  size_t hashLen;

  SslInterface::sslSHA512(key.c_str(), key.size(), hash, hashLen);

  // as hex
  char* hex = nullptr;
  size_t hexLen;

  SslInterface::sslHEX(hash, hashLen, hex, hexLen);

  delete[] hash;

  // and return
  v8::Handle<v8::String> hashStr = TRI_V8_PAIR_STRING(isolate, hex, (int)hexLen);

  delete[] hex;

  TRI_V8_RETURN(hashStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the sha384 sum
///
/// @FUN{internal.sha384(@FA{text})}
///
/// Computes an sha384 for the @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Sha384(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("sha384(<text>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);

  // create sha384
  char* hash = nullptr;
  size_t hashLen;

  SslInterface::sslSHA384(key.c_str(), key.size(), hash, hashLen);

  // as hex
  char* hex = nullptr;
  size_t hexLen;

  SslInterface::sslHEX(hash, hashLen, hex, hexLen);

  delete[] hash;

  // and return
  v8::Handle<v8::String> hashStr = TRI_V8_PAIR_STRING(isolate, hex, (int)hexLen);

  delete[] hex;

  TRI_V8_RETURN(hashStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the sha256 sum
///
/// @FUN{internal.sha256(@FA{text})}
///
/// Computes an sha256 for the @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Sha256(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("sha256(<text>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);

  // create sha256
  char* hash = nullptr;
  size_t hashLen;

  SslInterface::sslSHA256(key.c_str(), key.size(), hash, hashLen);

  // as hex
  char* hex = nullptr;
  size_t hexLen;

  SslInterface::sslHEX(hash, hashLen, hex, hexLen);

  delete[] hash;

  // and return
  v8::Handle<v8::String> hashStr = TRI_V8_PAIR_STRING(isolate, hex, (int)hexLen);

  delete[] hex;

  TRI_V8_RETURN(hashStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the sha224 sum
///
/// @FUN{internal.sha224(@FA{text})}
///
/// Computes an sha224 for the @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Sha224(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("sha224(<text>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);

  // create sha224
  char* hash = nullptr;
  size_t hashLen;

  SslInterface::sslSHA224(key.c_str(), key.size(), hash, hashLen);

  // as hex
  char* hex = nullptr;
  size_t hexLen;

  SslInterface::sslHEX(hash, hashLen, hex, hexLen);

  delete[] hash;

  // and return
  v8::Handle<v8::String> hashStr = TRI_V8_PAIR_STRING(isolate, hex, (int)hexLen);

  delete[] hex;

  TRI_V8_RETURN(hashStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the sha1 sum
///
/// @FUN{internal.sha1(@FA{text})}
///
/// Computes an sha1 for the @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_Sha1(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 1 || !args[0]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("sha1(<text>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);

  // create sha1
  char* hash = nullptr;
  size_t hashLen;

  SslInterface::sslSHA1(key.c_str(), key.size(), hash, hashLen);

  // as hex
  char* hex = nullptr;
  size_t hexLen;

  SslInterface::sslHEX(hash, hashLen, hex, hexLen);

  delete[] hash;

  // and return
  v8::Handle<v8::String> hashStr = TRI_V8_PAIR_STRING(isolate, hex, (int)hexLen);

  delete[] hex;

  TRI_V8_RETURN(hashStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief private key signing of a message
///
/// @FUN{internal.rsaprivsign(@FA{text})}
///
/// RSA signs a message @FA{text}.
////////////////////////////////////////////////////////////////////////////////

static void JS_RsaPrivSign(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 2 || !args[0]->IsString()|| !args[1]->IsString()) {
    TRI_V8_THROW_EXCEPTION_USAGE("privsign(<private key pem>, <to sign>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);
  std::string message = TRI_ObjectToString(isolate, args[1]);
  std::string sign;
  std::string error;

  auto res = SslInterface::rsaPrivSign(key, message, sign, error);

  if (res == 0) {
    sign = StringUtils::encodeBase64(sign);
  } else {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_FAILED, error);
  }

  v8::Handle<v8::String> signStr = TRI_V8_PAIR_STRING(isolate, sign.c_str(), (int)sign.size());

  TRI_V8_RETURN(signStr);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sleeps
///
/// @FUN{internal.sleep(@FA{seconds})}
///
/// Wait for @FA{seconds}, without calling the garbage collection.
////////////////////////////////////////////////////////////////////////////////

static void JS_Sleep(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  if (isExecutionDeadlineReached(isolate)) {
    return;
  }

  // extract arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("sleep(<seconds>)");
  }

  double n = correctTimeoutToExecutionDeadlineS(TRI_ObjectToDouble(isolate, args[0]));

  TRI_GET_GLOBALS();
  Result res = ::doSleep(n, v8g->_server);
  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res.errorNumber());
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the current time
///
/// @FUN{internal.time()}
///
/// Returns the current time in seconds.
///
/// @verbinclude fluent36
////////////////////////////////////////////////////////////////////////////////

static void JS_Time(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  TRI_V8_RETURN(v8::Number::New(isolate, TRI_microtime()));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief waits for the specified amount of time and calls the garbage
/// collection.
///
/// @FUN{internal.wait(@FA{seconds})}
///
/// Wait for @FA{seconds}, call the garbage collection.
////////////////////////////////////////////////////////////////////////////////

static void JS_Wait(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  if (isExecutionDeadlineReached(isolate)) {
    return;
  }

  // extract arguments
  if (args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("wait(<seconds>, <gc>)");
  }

  double n = correctTimeoutToExecutionDeadlineS(TRI_ObjectToDouble(isolate, args[0]));

  bool gc = true;  // default is to trigger the gc
  if (args.Length() > 1) {
    gc = TRI_ObjectToBoolean(isolate, args[1]);
  }

  if (gc) {
    TRI_RunGarbageCollectionV8(isolate, n);
  }

  // wait without gc
  TRI_GET_GLOBALS();
  Result res = ::doSleep(n, v8g->_server);
  if (res.fail()) {
    TRI_V8_THROW_EXCEPTION(res.errorNumber());
  }

  TRI_V8_RETURN_UNDEFINED();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns whether failure points can be used
///
/// @FUN{internal.debugCanUseFailAt()}
///
/// Returns whether failure points can be be used
////////////////////////////////////////////////////////////////////////////////

static void JS_DebugCanUseFailAt(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("debugCanUseFailAt()");
  }

  if (TRI_CanUseFailurePointsDebugging()) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the PBKDF2 HMAC SHA1 derived key
///
/// @FUN{internal.PBKDF2HS1(@FA{salt}, @FA{password}, @FA{iterations},
/// @FA{keyLength})}
///
/// Computes the PBKDF2 HMAC SHA1 derived key for the @FA{password}.
////////////////////////////////////////////////////////////////////////////////

static void JS_PBKDF2HS1(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() < 4 || !args[0]->IsString() || !args[1]->IsString() ||
      !args[2]->IsNumber() || !args[3]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "PBKDF2(<salt>, <password>, <iterations>, <keyLength>)");
  }

  std::string salt = TRI_ObjectToString(isolate, args[0]);
  std::string password = TRI_ObjectToString(isolate, args[1]);
  int iterations = (int)TRI_ObjectToInt64(isolate, args[2]);
  int keyLength = (int)TRI_ObjectToInt64(isolate, args[3]);

  std::string result =
      SslInterface::sslPBKDF2HS1(salt.c_str(), salt.size(), password.c_str(),
                                 password.size(), iterations, keyLength);
  TRI_V8_RETURN_STD_STRING(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the PBKDF2 HMAC derived key
///
/// @FUN{internal.PBKDF2(@FA{salt}, @FA{password}, @FA{iterations},
/// @FA{keyLength}, @FA{algorithm})}
///
/// Computes the PBKDF2 HMAC derived key for the @FA{password}.
////////////////////////////////////////////////////////////////////////////////

static void JS_PBKDF2(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() < 4 || !args[0]->IsString() || !args[1]->IsString() ||
      !args[2]->IsNumber() || !args[3]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "PBKDF2_SHA(<salt>, <password>, <iterations>, <keyLength>, "
        "<algorithm>)");
  }

  SslInterface::Algorithm al = SslInterface::Algorithm::ALGORITHM_SHA1;
  if (args.Length() > 4 && !args[4]->IsUndefined()) {
    std::string algorithm = TRI_ObjectToString(isolate, args[4]);
    StringUtils::tolowerInPlace(algorithm);

    if (algorithm == "sha1") {
      al = SslInterface::Algorithm::ALGORITHM_SHA1;
    } else if (algorithm == "sha512") {
      al = SslInterface::Algorithm::ALGORITHM_SHA512;
    } else if (algorithm == "sha384") {
      al = SslInterface::Algorithm::ALGORITHM_SHA384;
    } else if (algorithm == "sha256") {
      al = SslInterface::Algorithm::ALGORITHM_SHA256;
    } else if (algorithm == "sha224") {
      al = SslInterface::Algorithm::ALGORITHM_SHA224;
    } else if (algorithm == "md5") {
      al = SslInterface::Algorithm::ALGORITHM_MD5;
    } else {
      TRI_V8_THROW_EXCEPTION_PARAMETER("invalid value for <algorithm>");
    }
  }

  std::string salt = TRI_ObjectToString(isolate, args[0]);
  std::string password = TRI_ObjectToString(isolate, args[1]);
  int iterations = (int)TRI_ObjectToInt64(isolate, args[2]);
  int keyLength = (int)TRI_ObjectToInt64(isolate, args[3]);

  std::string result =
      SslInterface::sslPBKDF2(salt.c_str(), salt.size(), password.c_str(),
                              password.size(), iterations, keyLength, al);
  TRI_V8_RETURN_STD_STRING(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief computes the HMAC signature
///
/// @FUN{internal.HMAC(@FA{key}, @FA{message}, @FA{algorithm})}
///
/// Computes the HMAC for the @FA{message}.
////////////////////////////////////////////////////////////////////////////////

static void JS_HMAC(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract arguments
  if (args.Length() < 2) {
    TRI_V8_THROW_EXCEPTION_USAGE("HMAC(<key>, <text>, <algorithm>)");
  }

  std::string key = TRI_ObjectToString(isolate, args[0]);
  std::string message = TRI_ObjectToString(isolate, args[1]);

  SslInterface::Algorithm al = SslInterface::Algorithm::ALGORITHM_SHA256;
  if (args.Length() > 2 && !args[2]->IsUndefined()) {
    std::string algorithm = TRI_ObjectToString(isolate, args[2]);
    StringUtils::tolowerInPlace(algorithm);

    if (algorithm == "sha1") {
      al = SslInterface::Algorithm::ALGORITHM_SHA1;
    } else if (algorithm == "sha512") {
      al = SslInterface::Algorithm::ALGORITHM_SHA512;
    } else if (algorithm == "sha384") {
      al = SslInterface::Algorithm::ALGORITHM_SHA384;
    } else if (algorithm == "sha256") {
      al = SslInterface::Algorithm::ALGORITHM_SHA256;
    } else if (algorithm == "sha224") {
      al = SslInterface::Algorithm::ALGORITHM_SHA224;
    } else if (algorithm == "md5") {
      al = SslInterface::Algorithm::ALGORITHM_MD5;
    } else {
      TRI_V8_THROW_EXCEPTION_PARAMETER("invalid value for <algorithm>");
    }
  }

  std::string v = SslInterface::sslHMAC(key.c_str(), key.size(),
                                        message.c_str(), message.size(), al);

  std::string result = StringUtils::encodeHex(v.data(), v.size());
  TRI_V8_RETURN_STD_STRING(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Convert programm stati to V8
////////////////////////////////////////////////////////////////////////////////
static char const* convertProcessStatusToString(TRI_external_status_e processStatus) {
  char const* status = "UNKNOWN";

  switch (processStatus) {
    case TRI_EXT_NOT_STARTED:
      status = "NOT-STARTED";
      break;
    case TRI_EXT_PIPE_FAILED:
      status = "FAILED";
      break;
    case TRI_EXT_FORK_FAILED:
      status = "FAILED";
      break;
    case TRI_EXT_RUNNING:
      status = "RUNNING";
      break;
    case TRI_EXT_NOT_FOUND:
      status = "NOT-FOUND";
      break;
    case TRI_EXT_TERMINATED:
      status = "TERMINATED";
      break;
    case TRI_EXT_ABORTED:
      status = "ABORTED";
      break;
    case TRI_EXT_STOPPED:
      status = "STOPPED";
      break;
    case TRI_EXT_TIMEOUT:
      status = "TIMEOUT";
      break;
  }
  return status;
}

static void convertPipeStatus(v8::FunctionCallbackInfo<v8::Value> const& args,
                              v8::Handle<v8::Object>& result, ExternalId const& external) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  auto context = TRI_IGETC;

  result->Set(context,
              TRI_V8_ASCII_STRING(isolate, "pid"),
              v8::Number::New(isolate, external._pid)).FromMaybe(false);

  // Now report about possible stdin and stdout pipes:
#ifndef _WIN32
  if (external._readPipe >= 0) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "readPipe"),
                v8::Number::New(isolate, external._readPipe)).FromMaybe(false);
  }
  if (external._writePipe >= 0) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "writePipe"),
                v8::Number::New(isolate, external._writePipe)).FromMaybe(false);
  }
#else
  if (external._readPipe != INVALID_HANDLE_VALUE) {
    auto fn = getFileNameFromHandle(external._readPipe);
    if (fn.length() > 0) {
      result->Set(context,
                  TRI_V8_ASCII_STRING(isolate, "readPipe"),
                  TRI_V8_STD_STRING(isolate, fn)).FromMaybe(false);
    }
  }
  if (external._writePipe != INVALID_HANDLE_VALUE) {
    auto fn = getFileNameFromHandle(external._writePipe);
    if (fn.length() > 0) {
      result->Set(context,
                  TRI_V8_ASCII_STRING(isolate, "writePipe"),
                  TRI_V8_STD_STRING(isolate, fn)).FromMaybe(false);
    }
  }
#endif
  TRI_V8_TRY_CATCH_END;
}

static void convertStatusToV8(v8::FunctionCallbackInfo<v8::Value> const& args,
                              v8::Handle<v8::Object>& result,
                              ExternalProcessStatus const& external_status,
                              ExternalId const& external) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  auto context = TRI_IGETC;
  convertPipeStatus(args, result, external);

  result->Set(context,
              TRI_V8_ASCII_STRING(isolate, "status"),
              TRI_V8_ASCII_STRING(isolate, convertProcessStatusToString(
                                               external_status._status))).FromMaybe(false);

  if (external_status._status == TRI_EXT_TERMINATED) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "exit"),
                v8::Integer::New(isolate, static_cast<int32_t>(external_status._exitStatus))).FromMaybe(false);
  } else if (external_status._status == TRI_EXT_ABORTED) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "signal"),
                v8::Integer::New(isolate, static_cast<int32_t>(external_status._exitStatus))).FromMaybe(false);
  }
  if (external_status._errorMessage.length() > 0) {
    result->Set(context,
                TRI_V8_STD_STRING(isolate, StaticStrings::ErrorMessage),
                TRI_V8_STD_STRING(isolate, external_status._errorMessage)).FromMaybe(false);
  }
  TRI_V8_TRY_CATCH_END;
}

static void convertProcessInfoToV8(v8::FunctionCallbackInfo<v8::Value> const& args,
                                   v8::Handle<v8::Object>& result,
                                   ExternalProcess const& external_process) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  auto context = TRI_IGETC;
  convertPipeStatus(args, result, external_process);

  result->Set(context,
              TRI_V8_ASCII_STRING(isolate, "status"),
              TRI_V8_ASCII_STRING(isolate, convertProcessStatusToString(
                                               external_process._status))).FromMaybe(false);

  if (external_process._status == TRI_EXT_TERMINATED) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "exit"),
                v8::Integer::New(isolate, static_cast<int32_t>(external_process._exitStatus))).FromMaybe(false);
  } else if (external_process._status == TRI_EXT_ABORTED) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "signal"),
                v8::Integer::New(isolate, static_cast<int32_t>(external_process._exitStatus))).FromMaybe(false);
  }
  result->Set(context,
              TRI_V8_ASCII_STRING(isolate, "executable"),
              TRI_V8_STD_STRING(isolate, external_process._executable)).FromMaybe(false);

  v8::Handle<v8::Array> arguments =
      v8::Array::New(isolate, static_cast<int>(external_process._numberArguments));
  for (uint32_t i = 0; i < external_process._numberArguments; i++) {
    arguments->Set(context,
                   i, TRI_V8_ASCII_STRING(isolate, external_process._arguments[i])).FromMaybe(false);
  }
  result->Set(context,
              TRI_V8_ASCII_STRING(isolate, "arguments"),
              arguments).FromMaybe(false);
  TRI_V8_TRY_CATCH_END;
}
////////////////////////////////////////////////////////////////////////////////
/// @brief lists all running external processes
////////////////////////////////////////////////////////////////////////////////

static void JS_GetExternalSpawned(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  // extract the arguments
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("getExternalSpawned()");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  v8::Handle<v8::Array> spawnedProcesses =
      v8::Array::New(isolate, static_cast<int>(ExternalProcesses.size()));

  uint32_t i = 0;
  for (auto const& process : ExternalProcesses) {
    v8::Handle<v8::Object> oneProcess = v8::Object::New(isolate);
    convertProcessInfoToV8(args, oneProcess, *process);
    spawnedProcesses->Set(context, i, oneProcess).FromMaybe(false);
    i++;
  }

  TRI_V8_RETURN(spawnedProcesses);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a external program
////////////////////////////////////////////////////////////////////////////////

static void JS_ExecuteExternal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  // extract the arguments
  if (5 < args.Length() || args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "executeExternal(<filename>[, <arguments> [,<usePipes> [,<env> [, <workingDirectory> ] ] ] ])");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a string");
  }

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  std::vector<std::string> arguments;

  if (2 <= args.Length()) {
    v8::Handle<v8::Value> a = args[1];

    if (a->IsArray()) {
      v8::Handle<v8::Array> arr = v8::Handle<v8::Array>::Cast(a);

      uint32_t n = arr->Length();

      for (uint32_t i = 0; i < n; ++i) {
        TRI_Utf8ValueNFC arg(isolate, arr->Get(context, i).FromMaybe(v8::Local<v8::Value>()));

        if (*arg == nullptr) {
          arguments.push_back("");
        } else {
          arguments.push_back(*arg);
        }
      }
    } else {
      TRI_Utf8ValueNFC arg(isolate, a);

      if (*arg == nullptr) {
        arguments.push_back("");
      } else {
        arguments.push_back(*arg);
      }
    }
  }

  bool usePipes = false;

  if (3 <= args.Length()) {
    usePipes = TRI_ObjectToBoolean(isolate, args[2]);
  }

  std::vector<std::string> additionalEnv;

  if (4 <= args.Length()) {
    v8::Handle<v8::Value> a = args[3];

    if (a->IsArray()) {
      v8::Handle<v8::Array> arr = v8::Handle<v8::Array>::Cast(a);

      uint32_t n = arr->Length();

      for (uint32_t i = 0; i < n; ++i) {
        TRI_Utf8ValueNFC arg(isolate, arr->Get(context, i).FromMaybe(v8::Handle<v8::Value>()));

        if (*arg == nullptr) {
          additionalEnv.push_back("");
        } else {
          additionalEnv.push_back(*arg);
        }
      }
    }
  }

  auto workingDirectory = FileUtils::currentDirectory().result();
  std::string subProcessWorkingDirectory = workingDirectory;

  if (5 <= args.Length()) {
    TRI_Utf8ValueNFC name(isolate, args[4]);
    if (*name == nullptr) {
      TRI_V8_THROW_TYPE_ERROR("<workingDirectory> must be a string");
    }

    subProcessWorkingDirectory = std::string(*name, name.length());
  }
  ExternalId external;
  if (subProcessWorkingDirectory != workingDirectory) {
    FileUtils::changeDirectory(subProcessWorkingDirectory);
  }
  TRI_CreateExternalProcess(*name, arguments, additionalEnv, usePipes, &external);
  FileUtils::changeDirectory(workingDirectory);

  if (external._pid == TRI_INVALID_PROCESS_ID) {
    TRI_V8_THROW_ERROR("Process could not be started");
  }
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  convertPipeStatus(args, result, external);

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the status of an external process
////////////////////////////////////////////////////////////////////////////////

static void JS_StatusExternal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() < 1 || args.Length() > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "statusExternal(<external-identifier>[, <wait>[, <timeoutms>]])");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  ExternalId pid;

  pid._pid = static_cast<TRI_pid_t>(TRI_ObjectToUInt64(isolate, args[0], true));
  bool wait = false;
  if (args.Length() >= 2) {
    wait = TRI_ObjectToBoolean(isolate, args[1]);
  }
  uint32_t timeoutms = 0;
  if (args.Length() >= 2) {
    timeoutms = static_cast<uint32_t>(TRI_ObjectToUInt64(isolate, args[2], true));
  }

  ExternalProcessStatus external = TRI_CheckExternalProcess(pid, wait, timeoutms);

  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  auto context = TRI_IGETC;

  result->Set(context,
              TRI_V8_ASCII_STRING(isolate, "status"),
              TRI_V8_STRING(isolate, convertProcessStatusToString(external._status))).FromMaybe(false);

  if (external._status == TRI_EXT_TERMINATED) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "exit"),
                v8::Integer::New(isolate, static_cast<int32_t>(external._exitStatus))).FromMaybe(false);
  } else if (external._status == TRI_EXT_ABORTED) {
    result->Set(context,
                TRI_V8_ASCII_STRING(isolate, "signal"),
                v8::Integer::New(isolate, static_cast<int32_t>(external._exitStatus))).FromMaybe(false);
  }
  if (external._errorMessage.length() > 0) {
    result->Set(context,
                TRI_V8_STD_STRING(isolate, StaticStrings::ErrorMessage),
                TRI_V8_STD_STRING(isolate, external._errorMessage)).FromMaybe(false);
  }
  // return the result
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a external program
////////////////////////////////////////////////////////////////////////////////

static void JS_ExecuteExternalAndWait(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  // extract the arguments
  if (6 < args.Length() || args.Length() < 1) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "executeExternalAndWait(<filename>[, <arguments> [,<usePipes>, [, <timeoutms> [, <env> [, <workingDirectory> ] ] ] ] ])");
  }

  TRI_Utf8ValueNFC name(isolate, args[0]);

  if (*name == nullptr) {
    TRI_V8_THROW_TYPE_ERROR("<filename> must be a string");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  if (!v8security.isAllowedToAccessPath(isolate, *name, FSAccessType::READ)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FORBIDDEN,
                                   std::string("not allowed to read files in this path: ") + *name);
  }

  std::vector<std::string> arguments;

  if (2 <= args.Length()) {
    v8::Handle<v8::Value> a = args[1];

    if (a->IsArray()) {
      v8::Handle<v8::Array> arr = v8::Handle<v8::Array>::Cast(a);

      uint32_t const n = arr->Length();

      for (uint32_t i = 0; i < n; ++i) {
        TRI_Utf8ValueNFC arg(isolate, arr->Get(context, i).FromMaybe(v8::Handle<v8::Value>()));

        if (*arg == nullptr) {
          arguments.push_back("");
        } else {
          arguments.push_back(*arg);
        }
      }
    } else {
      TRI_Utf8ValueNFC arg(isolate, a);

      if (*arg == nullptr) {
        arguments.push_back("");
      } else {
        arguments.push_back(*arg);
      }
    }
  }

  bool usePipes = false;
  if (args.Length() >= 3) {
    usePipes = TRI_ObjectToBoolean(isolate, args[2]);
  }

  uint32_t timeoutms = 0;
  if (args.Length() >= 4) {
    timeoutms = static_cast<uint32_t>(TRI_ObjectToUInt64(isolate, args[3], true));
  }

  std::vector<std::string> additionalEnv;

  if (5 <= args.Length()) {
    v8::Handle<v8::Value> a = args[4];

    if (a->IsArray()) {
      v8::Handle<v8::Array> arr = v8::Handle<v8::Array>::Cast(a);

      uint32_t n = arr->Length();

      for (uint32_t i = 0; i < n; ++i) {
        TRI_Utf8ValueNFC arg(isolate, arr->Get(context, i).FromMaybe(v8::Handle<v8::Value>()));

        if (*arg == nullptr) {
          additionalEnv.push_back("");
        } else {
          additionalEnv.push_back(*arg);
        }
      }
    }
  }

  auto workingDirectory = FileUtils::currentDirectory().result();
  std::string subProcessWorkingDirectory = workingDirectory;

  if (5 <= args.Length()) {
    TRI_Utf8ValueNFC name(isolate, args[4]);
    if (*name == nullptr) {
      TRI_V8_THROW_TYPE_ERROR("<workingDirectory> must be a string");
    }

    subProcessWorkingDirectory = std::string(*name, name.length());
  }
  ExternalId external;
  if (subProcessWorkingDirectory != workingDirectory) {
    FileUtils::changeDirectory(subProcessWorkingDirectory);
  }
  TRI_CreateExternalProcess(*name, arguments, additionalEnv, usePipes, &external);
  FileUtils::changeDirectory(workingDirectory);

  if (external._pid == TRI_INVALID_PROCESS_ID) {
    TRI_V8_THROW_ERROR("Process could not be started");
  }
  v8::Handle<v8::Object> result = v8::Object::New(isolate);

  ExternalId pid;
  pid._pid = external._pid;

  auto external_status = TRI_CheckExternalProcess(pid, true, timeoutms);

  convertStatusToV8(args, result, external_status, external);

  // return the result
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief kills an external process
////////////////////////////////////////////////////////////////////////////////

static void JS_KillExternal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() < 1 || args.Length() > 2) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "killExternal(<external-identifier>[[, <signal>], isTerminal])");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  int signal = SIGTERM;
  if (args.Length() >= 2) {
    signal = static_cast<int>(TRI_ObjectToInt64(isolate, args[1]));
  }

  bool isTerminating;
  if (args.Length() >= 3) {
    isTerminating = TRI_ObjectToBoolean(isolate, args[2]);
  } else {
    isTerminating = arangodb::signals::isDeadly(signal);
  }

  ExternalId pid;

#ifndef _WIN32
  pid._pid = static_cast<TRI_pid_t>(TRI_ObjectToUInt64(isolate, args[0], true));
#else
  pid._pid = static_cast<DWORD>(TRI_ObjectToUInt64(isolate, args[0], true));
#endif
  if (pid._pid == 0) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to kill 0");
  }

  // return the result
  v8::Handle<v8::Object> result = v8::Object::New(isolate);
  ExternalId external;
  external._pid = pid._pid;

  auto external_status = TRI_KillExternalProcess(pid, signal, isTerminating);

  convertStatusToV8(args, result, external_status, external);

  // return the result
  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief suspends an external process, only Unix
////////////////////////////////////////////////////////////////////////////////

static void JS_SuspendExternal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("suspendExternal(<external-identifier>)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  ExternalId pid;

#ifndef _WIN32
  pid._pid = static_cast<TRI_pid_t>(TRI_ObjectToUInt64(isolate, args[0], true));
#else
  pid._pid = static_cast<DWORD>(TRI_ObjectToUInt64(isolate, args[0], true));
#endif

  // return the result
  if (TRI_SuspendExternalProcess(pid)) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief continues an external process
////////////////////////////////////////////////////////////////////////////////

static void JS_ContinueExternal(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("continueExternal(<external-identifier>)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToControlProcesses(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to execute or modify state of external processes");
  }

  ExternalId pid;

#ifndef _WIN32
  pid._pid = static_cast<TRI_pid_t>(TRI_ObjectToUInt64(isolate, args[0], true));
#else
  pid._pid = static_cast<DWORD>(TRI_ObjectToUInt64(isolate, args[0], true));
#endif

  // return the result
  if (TRI_ContinueExternalProcess(pid)) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if a port is available
////////////////////////////////////////////////////////////////////////////////

static void JS_TestPort(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("testPort(<address>)");
  }

  TRI_GET_GLOBALS();
  V8SecurityFeature& v8security = v8g->_server.getFeature<V8SecurityFeature>();

  if (!v8security.isAllowedToTestPorts(isolate)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_FORBIDDEN,
        "not allowed to test ports");
  }

  std::string address = TRI_ObjectToString(isolate, args[0]);
  Endpoint* endpoint = Endpoint::serverFactory(address, 10, false);
  if (nullptr == endpoint) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "address description invalid, cannot create endpoint");
  }
  TRI_socket_t s = endpoint->connect(1, 1);
  bool available = TRI_isvalidsocket(s);

  if (available) {
    endpoint->disconnect();
  }

  delete endpoint;

  // return the result
  if (available) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if the application server is stopping
////////////////////////////////////////////////////////////////////////////////

static void JS_IsStopping(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the arguments
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("isStopping()");
  }

  TRI_GET_GLOBALS();
  if (v8g->_server.isStopping()) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns keycode of currently pressed key
////////////////////////////////////////////////////////////////////////////////

static void JS_termsize(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  // extract the arguments
  if (args.Length() != 0) {
    TRI_V8_THROW_EXCEPTION_USAGE("termsize()");
  }

  TRI_TerminalSize s = TRI_DefaultTerminalSize();
  v8::Handle<v8::Array> list = v8::Array::New(isolate, 2);
  list->Set(context, 0, v8::Integer::New(isolate, s.rows)).FromMaybe(false);
  list->Set(context, 1, v8::Integer::New(isolate, s.columns)).FromMaybe(false);

  TRI_V8_RETURN(list);
  TRI_V8_TRY_CATCH_END
}

/// @brief convert a V8 value to VPack
static void JS_V8ToVPack(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the argument
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("V8_TO_VPACK(value)");
  }

  VPackBuilder builder;
  TRI_V8ToVPack(isolate, builder, args[0], false);

  VPackSlice slice = builder.slice();

  V8Buffer* buffer =
      V8Buffer::New(isolate, slice.startAs<char const>(), slice.byteSize());
  v8::Local<v8::Object> bufferObject =
      v8::Local<v8::Object>::New(isolate, buffer->_handle);
  TRI_V8_RETURN(bufferObject);

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

/// @brief convert a VPack value to V8
static void JS_VPackToV8(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  // extract the argument
  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("VPACK_TO_V8(value)");
  }

  VPackValidator validator;

  if (args[0]->IsString() || args[0]->IsStringObject()) {
    // supplied argument is a string
    std::string const value = TRI_ObjectToString(isolate, args[0]);

    validator.validate(value.c_str(), value.size(), false);

    VPackSlice slice(reinterpret_cast<uint8_t const*>(value.data()));
    v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, slice);
    TRI_V8_RETURN(result);
  } else if (args[0]->IsObject() && V8Buffer::hasInstance(isolate, args[0])) {
    // argument is a buffer
    char const* data = V8Buffer::data(isolate, args[0].As<v8::Object>());
    size_t size = V8Buffer::length(isolate, args[0].As<v8::Object>());

    validator.validate(data, size, false);

    VPackSlice slice(reinterpret_cast<uint8_t const*>(data));
    v8::Handle<v8::Value> result = TRI_VPackToV8(isolate, slice);
    TRI_V8_RETURN(result);
  } else {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "invalid argument type for VPACK_TO_V8()");
  }

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ArangoError
////////////////////////////////////////////////////////////////////////////////

static void JS_ArangoError(v8::FunctionCallbackInfo<v8::Value> const& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  TRI_GET_GLOBALS();
  TRI_GET_GLOBAL_STRING(ErrorKey);
  TRI_GET_GLOBAL_STRING(ErrorNumKey);

  v8::Handle<v8::Object> self =
      args.Holder()->ToObject(TRI_IGETC).FromMaybe(v8::Local<v8::Object>());

  self->Set(context, ErrorKey, v8::True(isolate)).FromMaybe(false);
  self->Set(context, ErrorNumKey,
            v8::Integer::New(isolate, static_cast<int>(TRI_ERROR_FAILED)))
      .FromMaybe(false);

  if (0 < args.Length() && args[0]->IsObject()) {
    TRI_GET_GLOBAL_STRING(CodeKey);
    TRI_GET_GLOBAL_STRING(ErrorMessageKey);

    v8::Handle<v8::Object> data = TRI_ToObject(context, args[0]);

    if (TRI_HasProperty(context, isolate, data, ErrorKey)) {
      self->Set(context, ErrorKey, TRI_GetProperty(context, isolate, data, ErrorKey)).FromMaybe(false);
    }

    if (TRI_HasProperty(context, isolate, data, CodeKey)) {
      self->Set(context, CodeKey, TRI_GetProperty(context, isolate, data, CodeKey)).FromMaybe(false);
    }

    if (TRI_HasProperty(context, isolate, data, ErrorNumKey)) {
      self->Set(context, ErrorNumKey, TRI_GetProperty(context, isolate, data, ErrorNumKey)).FromMaybe(false);
    }

    if (TRI_HasProperty(context, isolate, data, ErrorMessageKey)) {
      self->Set(context, ErrorMessageKey, TRI_GetProperty(context, isolate, data, ErrorMessageKey)).FromMaybe(false);
    }
  }

  {
    // call Error.captureStackTrace(this) so the ArangoError object gets a nifty
    // stack trace
    v8::Handle<v8::Object> current = isolate->GetCurrentContext()->Global();
    v8::Handle<v8::Value> errorObject =
        TRI_GetProperty(context, isolate, current, "Error");
    v8::Handle<v8::Object> err = v8::Handle<v8::Object>::Cast(errorObject);
    v8::Handle<v8::Function> captureStackTrace = v8::Handle<v8::Function>::Cast(
        TRI_GetProperty(context, isolate, err, "captureStackTrace"));

    v8::Handle<v8::Value> arguments[] = {self};
    captureStackTrace->Call(TRI_IGETC, current, 1, arguments).FromMaybe(v8::Local<v8::Value>());
  }

  TRI_V8_RETURN(self);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief isIP
////////////////////////////////////////////////////////////////////////////////

static void JS_IsIP(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("isIP(<value>)");
  }

  TRI_Utf8ValueNFC address(isolate, args[0]);

  if (TRI_InetPton4(*address, nullptr) == TRI_ERROR_NO_ERROR) {
    TRI_V8_RETURN(v8::Number::New(isolate, 4));
  } else if (TRI_InetPton6(*address, nullptr) == TRI_ERROR_NO_ERROR) {
    TRI_V8_RETURN(v8::Number::New(isolate, 6));
  } else {
    TRI_V8_RETURN(v8::Number::New(isolate, 0));
  }
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief SplitWordlist - splits words via the tokenizer
////////////////////////////////////////////////////////////////////////////////

static void JS_SplitWordlist(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
  auto context = TRI_IGETC;

  if ((args.Length() < 2) || (args.Length() > 4)) {
    TRI_V8_THROW_EXCEPTION_USAGE(
        "SplitWordlist(<value>, minLength, [<maxLength>, [<lowerCase>]])");
  }

  std::string stringToTokenize = TRI_ObjectToString(isolate, args[0]);

  size_t minLength = static_cast<size_t>(TRI_ObjectToUInt64(isolate, args[1], true));

  size_t maxLength = 40;
  if (args.Length() > 2) {
    maxLength = static_cast<size_t>(TRI_ObjectToUInt64(isolate, args[2], true));
  }

  bool lowerCase = false;
  if (args.Length() > 3) {
    lowerCase = TRI_ObjectToBoolean(isolate, args[3]);
  }

  std::set<std::string> wordList;

  if (!Utf8Helper::DefaultUtf8Helper.tokenize(wordList, std::string_view(stringToTokenize),
                                              minLength, maxLength, lowerCase)) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "SplitWordlist failed!");
  }

  v8::Handle<v8::Array> v8WordList =
      v8::Array::New(isolate, static_cast<int>(wordList.size()));

  uint32_t i = 0;
  for (std::string const& word : wordList) {
    v8::Handle<v8::String> oneWord = TRI_V8_STD_STRING(isolate, word);
    v8WordList->Set(context, i, oneWord).FromMaybe(false);
    i++;
  }

  TRI_V8_RETURN(v8WordList);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reports an exception
////////////////////////////////////////////////////////////////////////////////

std::string TRI_StringifyV8Exception(v8::Isolate* isolate, v8::TryCatch* tryCatch) {
  v8::HandleScope handle_scope(isolate);

  TRI_Utf8ValueNFC exception(isolate, tryCatch->Exception());
  char const* exceptionString = *exception;
  v8::Handle<v8::Message> message = tryCatch->Message();
  std::string result;

  // V8 didn't provide any extra information about this error; just print the
  // exception.
  if (message.IsEmpty()) {
    if (exceptionString == nullptr) {
      result = "JavaScript exception\n";
    } else {
      result = "JavaScript exception: " + std::string(exceptionString) + "\n";
    }
  } else {
    TRI_Utf8ValueNFC filename(isolate, message->GetScriptResourceName());
    char const* filenameString = *filename;
    int linenum = message->GetLineNumber(TRI_IGETC).FromMaybe(-1);
    int start = message->GetStartColumn(TRI_IGETC).FromMaybe(-2) + 1;
    int end = message->GetEndColumn(TRI_IGETC).FromMaybe(-1);

    if ((filenameString == nullptr) || (!strcmp(filenameString, TRI_V8_SHELL_COMMAND_NAME))) {
      if (exceptionString == nullptr) {
        result = "JavaScript exception\n";
      } else {
        result = "JavaScript exception: " + std::string(exceptionString) + "\n";
      }
    } else {
      if (exceptionString == nullptr) {
        result = "JavaScript exception in file '" + std::string(filenameString) +
                 "' at " + StringUtils::itoa(linenum) + "," +
                 StringUtils::itoa(start) + "\n";
      } else {
        result = "JavaScript exception in file '" + std::string(filenameString) +
                 "' at " + StringUtils::itoa(linenum) + "," +
                 StringUtils::itoa(start) + ": " + exceptionString + "\n";
      }
    }

    TRI_Utf8ValueNFC sourceline(isolate, message->GetSourceLine(TRI_IGETC).FromMaybe(
                                             v8::Handle<v8::Value>()));

    if (*sourceline) {
      std::string l = *sourceline;

      result += "!" + l + "\n";

      if (1 < start) {
        l = std::string(start - 1, ' ');
      } else {
        l = "";
      }

      // in fact, we observed start being greater than end sometimes...
      // this does not make sense and seems to be a bug in V8, but it happens
      // so we need to work around this
      if (end >= start) {
        l += std::string((size_t)(end - start + 1), '^');
      } else {
        l = "^";
      }

      result += "!" + l + "\n";
    }

    auto stacktraceV8 =
        tryCatch->StackTrace(TRI_IGETC).FromMaybe(v8::Local<v8::Value>());
    TRI_Utf8ValueNFC stacktrace(isolate, stacktraceV8);

    if (*stacktrace && stacktrace.length() > 0) {
      result += "stacktrace: " + std::string(*stacktrace) + "\n";
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief prints an exception and stacktrace
////////////////////////////////////////////////////////////////////////////////

void TRI_LogV8Exception(v8::Isolate* isolate,
                        v8::TryCatch* tryCatch,
                        bool logStacktraceError) {
  v8::HandleScope handle_scope(isolate);

  TRI_Utf8ValueNFC exception(isolate, tryCatch->Exception());
  char const* exceptionString = *exception;
  v8::Handle<v8::Message> message = tryCatch->Message();

  // V8 didn't provide any extra information about this error; just print the
  // exception.
  if (message.IsEmpty()) {
    if (!isolate->IsExecutionTerminating()) {
      if (exceptionString == nullptr || *exceptionString == '\0') {
        LOG_TOPIC("49465", ERR, arangodb::Logger::FIXME)
            << "JavaScript exception";
      } else {
        TRI_ASSERT(exceptionString != nullptr);
        LOG_TOPIC("7e60e", ERR, arangodb::Logger::FIXME)
            << "JavaScript exception: " << exceptionString;
      }
    }
  } else {
    TRI_Utf8ValueNFC filename(isolate, message->GetScriptResourceName());
    char const* filenameString = *filename;
    // if ifdef is not used, the compiler will complain about linenum being
    // unused
    int linenum = message->GetLineNumber(TRI_IGETC).FromMaybe(-1);
    int start = message->GetStartColumn(TRI_IGETC).FromMaybe(-2) + 1;
    int end = message->GetEndColumn(TRI_IGETC).FromMaybe(-1);

    if (filenameString == nullptr) {
      if (exceptionString == nullptr) {
        LOG_TOPIC("27c91", ERR, arangodb::Logger::FIXME)
            << "JavaScript exception";
      } else {
        LOG_TOPIC("52220", ERR, arangodb::Logger::FIXME)
            << "JavaScript exception: " << exceptionString;
      }
    } else {
      if (exceptionString == nullptr) {
        LOG_TOPIC("b3488", ERR, arangodb::Logger::FIXME)
            << "JavaScript exception in file '" << filenameString << "' at "
            << linenum << "," << start;
      } else {
        LOG_TOPIC("8a210", ERR, arangodb::Logger::FIXME)
            << "JavaScript exception in file '" << filenameString << "' at "
            << linenum << "," << start << ": " << exceptionString;
      }
    }

    TRI_Utf8ValueNFC sourceline(isolate, message->GetSourceLine(TRI_IGETC).FromMaybe(
                                             v8::Handle<v8::Value>()));

    if (*sourceline) {
      std::string l = *sourceline;

      LOG_TOPIC("409ee", ERR, arangodb::Logger::FIXME) << "!" << l;

      if (1 < start) {
        l = std::string(start - 1, ' ');
      } else {
        l = "";
      }

      l += std::string((size_t)(end - start + 1), '^');

      LOG_TOPIC("cb0bd", ERR, arangodb::Logger::FIXME) << "!" << l;
    }
    auto stacktraceV8 =
      tryCatch->StackTrace(TRI_IGETC).FromMaybe(v8::Local<v8::Value>());
    TRI_Utf8ValueNFC stacktrace(isolate, stacktraceV8);

    if (*stacktrace && stacktrace.length() > 0) {
      if (logStacktraceError) {
        LOG_TOPIC("cb0c0", ERR, arangodb::Logger::V8) << "!" <<
          "stacktrace: " + std::string(*stacktrace) + "\n";
      } else {
        LOG_TOPIC("cb0bf", DEBUG, arangodb::Logger::V8) << "!" <<
          "stacktrace: " + std::string(*stacktrace) + "\n";
      }
    }

  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads a file into the current context
////////////////////////////////////////////////////////////////////////////////

bool TRI_ExecuteGlobalJavaScriptFile(v8::Isolate* isolate, char const* filename) {
  return LoadJavaScriptFile(isolate, filename, true);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a file
////////////////////////////////////////////////////////////////////////////////

bool TRI_ParseJavaScriptFile(v8::Isolate* isolate, char const* filename) {
  return LoadJavaScriptFile(isolate, filename, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a string within a V8 context, optionally print the result
////////////////////////////////////////////////////////////////////////////////

v8::Handle<v8::Value> TRI_ExecuteJavaScriptString(v8::Isolate* isolate,
                                                  v8::Handle<v8::Context> context,
                                                  v8::Handle<v8::String> const source,
                                                  v8::Handle<v8::String> const name,
                                                  bool printResult,
                                                  bool logExceptions,
                                                  bool logStacktraceError) {
  v8::EscapableHandleScope scope(isolate);

  v8::ScriptOrigin scriptOrigin(name);

  v8::Handle<v8::Value> result;
  v8::Handle<v8::Script> script =
      v8::Script::Compile(context, source, &scriptOrigin).FromMaybe(v8::Local<v8::Script>());

  // compilation failed, print errors that happened during compilation
  if (script.IsEmpty()) {
    return scope.Escape<v8::Value>(result);
  }

  // compilation succeeded, run the script
  result = script->Run(context).FromMaybe(v8::Local<v8::Value>());

  if (result.IsEmpty()) {
    return scope.Escape<v8::Value>(result);
  }

  // if all went well and the result wasn't undefined then print the returned
  // value
  if (printResult && !result->IsUndefined()) {
    v8::TryCatch tryCatch(isolate);

    v8::Handle<v8::String> printFuncName =
        TRI_V8_ASCII_STRING(isolate, "print");
    v8::Handle<v8::Function> print =
      v8::Handle<v8::Function>::Cast(context->Global()->Get(context, printFuncName).FromMaybe(v8::Local<v8::Value>()));

    if (print->IsFunction()) {
      v8::Handle<v8::Value> arguments[] = {result};
      print->Call(TRI_IGETC, print, 1, arguments).FromMaybe(v8::Local<v8::Value>());

      if (tryCatch.HasCaught()) {
        if (tryCatch.CanContinue()) {
          if (logExceptions) {
            TRI_LogV8Exception(isolate, &tryCatch, logStacktraceError);
          }
        } else {
          TRI_GET_GLOBALS();
          v8g->_canceled = true;
          return scope.Escape<v8::Value>(v8::Undefined(isolate));
        }
      }
    } else {
      LOG_TOPIC("837e5", ERR, arangodb::Logger::FIXME)
          << "no output function defined in JavaScript context";
    }
  }

  return scope.Escape<v8::Value>(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an error in a javascript object, based on arangodb::result
////////////////////////////////////////////////////////////////////////////////
void TRI_CreateErrorObject(v8::Isolate* isolate, arangodb::Result const& res) {
  TRI_CreateErrorObject(isolate, res.errorNumber(), res.errorMessage(), false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an error in a javascript object
////////////////////////////////////////////////////////////////////////////////

void TRI_CreateErrorObject(v8::Isolate* isolate, ErrorCode errorNumber,
                           std::string_view message, bool autoPrepend) {
  v8::HandleScope scope(isolate);

  try {
    // does string concatenation, so we must wrap this in a try...catch block
    if (autoPrepend && message.empty()) {
      CreateErrorObject(isolate, errorNumber,
                        basics::StringUtils::concatT(message, ": ",
                                                     TRI_errno_string(errorNumber)));
    } else {
      CreateErrorObject(isolate, errorNumber, message);
    }
  } catch (...) {
    // we cannot do anything about this here, but no C++ exception must
    // escape the C++ bindings called by V8
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief normalizes a v8 object
////////////////////////////////////////////////////////////////////////////////

void TRI_normalize_V8_Obj(v8::FunctionCallbackInfo<v8::Value> const& args,
                          v8::Handle<v8::Value> obj) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::HandleScope scope(isolate);

  v8::String::Value str(isolate, TRI_ObjectToString(context, obj));
  size_t str_len = str.length();
  if (str_len > 0) {
    UErrorCode errorCode = U_ZERO_ERROR;
    icu::Normalizer2 const* normalizer =
        icu::Normalizer2::getInstance(nullptr, "nfc", UNORM2_COMPOSE, errorCode);

    if (U_FAILURE(errorCode)) {
      TRI_V8_RETURN(TRI_V8_PAIR_STRING(isolate, (char*)*str, (int)str_len));
    }

    icu::UnicodeString result =
        normalizer->normalize(icu::UnicodeString((UChar*)*str, (int32_t)str_len), errorCode);

    if (U_FAILURE(errorCode)) {
      TRI_V8_RETURN(TRI_V8_STRING_UTF16(isolate, *str, (int)str_len));
    }

    // ..........................................................................
    // Take note here: we are assuming that the ICU type UChar is two bytes.
    // There is no guarantee that this will be the case on all platforms and
    // compilers. v8 expects uint16_t (2 bytes)
    // ..........................................................................

    TRI_V8_RETURN(TRI_V8_STRING_UTF16(isolate, (uint16_t const*)result.getBuffer(),
                                      result.length()));
  }

  TRI_V8_RETURN(v8::String::Empty(isolate));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates the path list
////////////////////////////////////////////////////////////////////////////////

v8::Handle<v8::Array> static V8PathList(v8::Isolate* isolate, std::string const& modules) {
  v8::EscapableHandleScope scope(isolate);
  auto context = TRI_IGETC;
#ifdef _WIN32
  std::vector<std::string> paths = StringUtils::split(modules, ';');
#else
  std::vector<std::string> paths = StringUtils::split(modules, ";:");
#endif

  uint32_t const n = static_cast<uint32_t>(paths.size());
  v8::Handle<v8::Array> result = v8::Array::New(isolate, n);

  for (uint32_t i = 0; i < n; ++i) {
    result->Set(context, i, TRI_V8_STD_STRING(isolate, paths[i])).FromMaybe(false);
  }

  return scope.Escape<v8::Array>(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief execute a single garbage collection run
////////////////////////////////////////////////////////////////////////////////

static bool SingleRunGarbageCollectionV8(v8::Isolate* isolate, int idleTimeInMs) {
  isolate->LowMemoryNotification();
  bool rc = isolate->IdleNotificationDeadline(idleTimeInMs);
  isolate->RunMicrotasks();
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief run the V8 garbage collection for at most a specifiable amount of
/// time. returns a boolean indicating whether or not the caller should attempt
/// to do more gc
////////////////////////////////////////////////////////////////////////////////

bool TRI_RunGarbageCollectionV8(v8::Isolate* isolate, double availableTime) {
  int idleTimeInMs = 1000;

  if (availableTime >= 5.0) {
    idleTimeInMs = 5000;
  }
  if (availableTime >= 10.0) {
    idleTimeInMs = 10000;
  }

  try {
    TRI_ClearObjectCacheV8(isolate);

    double const until = TRI_microtime() + availableTime;

    int totalGcTimeInMs = static_cast<int>(availableTime) * 1000;
    int gcAttempts = totalGcTimeInMs / idleTimeInMs;

    if (gcAttempts <= 0) {
      // minimum is to run the GC once
      gcAttempts = 1;
    }

    int gcTries = 0;

    while (++gcTries <= gcAttempts) {
      if (SingleRunGarbageCollectionV8(isolate, idleTimeInMs)) {
        return false;
      }
    }

    size_t i = 0;
    while (TRI_microtime() < until) {
      if (++i % 1024 == 0) {
        // garbage collection only every x iterations, otherwise we'll use too
        // much CPU
        if (++gcTries > gcAttempts || SingleRunGarbageCollectionV8(isolate, idleTimeInMs)) {
          return false;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
  } catch (...) {
    // error caught during garbage collection
    return false;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief clear the instance-local cache of wrapped objects
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearObjectCacheV8(v8::Isolate* isolate) {
  auto globals = isolate->GetCurrentContext()->Global();
  auto context = TRI_IGETC;

  if (TRI_HasProperty(context, isolate, globals, "__dbcache__")) {
    v8::Handle<v8::Object> cacheObject =
        TRI_GetProperty(context, isolate, globals, "__dbcache__")
            ->ToObject(TRI_IGETC)
            .FromMaybe(v8::Local<v8::Object>());
    if (!cacheObject.IsEmpty()) {
      v8::Handle<v8::Array> props = cacheObject->GetPropertyNames(TRI_IGETC).FromMaybe(v8::Local<v8::Array>());
      for (uint32_t i = 0; i < props->Length(); i++) {
        v8::Handle<v8::Value> key = props->Get(context, i).FromMaybe(v8::Handle<v8::Value>());
        cacheObject->Delete(context, key).FromMaybe(false);  // Ignore errors.
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check if we are in the Enterprise Edition
////////////////////////////////////////////////////////////////////////////////

static void JS_IsEnterprise(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);
#ifndef USE_ENTERPRISE
  TRI_V8_RETURN(v8::False(isolate));
#else
  TRI_V8_RETURN(v8::True(isolate));
#endif
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief convert error number to httpCode
////////////////////////////////////////////////////////////////////////////////

static void JS_ErrorNumberToHttpCode(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 1 || ! args[0]->IsNumber()) {
    TRI_V8_THROW_EXCEPTION_USAGE("errorNumberToHttpCode(<int>)");
  }

  auto num = TRI_ObjectToInt64(isolate, args[0]);
  auto code = arangodb::GeneralResponse::responseCode(ErrorCode{static_cast<int>(num)});

  using Type = typename std::underlying_type<arangodb::rest::ResponseCode>::type;
  TRI_V8_RETURN_INTEGER(static_cast<Type>(code));
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks wether database name is allowed or invalid
////////////////////////////////////////////////////////////////////////////////

static void JS_IsAllowedDatabaseName(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  if (args.Length() != 2 || ! args[0]->IsString() || ! args[1]->IsBoolean()) {
    TRI_V8_THROW_EXCEPTION_USAGE("IsAllowedDatabaseName(<string>, <bool>)");
  }

  auto databaseName = TRI_ObjectToString(isolate, args[0]);
  bool isExtendedName = TRI_ObjectToBoolean(isolate, args[1]);
  bool result = arangodb::DatabaseNameValidator::isAllowedName(true, isExtendedName, databaseName);

  TRI_V8_RETURN_BOOL(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stores the V8 utils functions inside the global variable
////////////////////////////////////////////////////////////////////////////////

extern void TRI_InitV8Env(v8::Isolate* isolate, v8::Handle<v8::Context> context);

void TRI_InitV8Utils(v8::Isolate* isolate,
                     v8::Handle<v8::Context> context,
                     std::string const& startupPath,
                     std::string const& modules) {
  v8::HandleScope scope(isolate);

  // check the isolate
  TRI_v8_global_t* v8g = TRI_GetV8Globals(isolate);
  TRI_ASSERT(v8g != nullptr);

  v8::Handle<v8::FunctionTemplate> ft;
  v8::Handle<v8::ObjectTemplate> rt;

  // .............................................................................
  // generate the general error template
  // .............................................................................

  ft = v8::FunctionTemplate::New(isolate, JS_ArangoError);
  ft->SetClassName(TRI_V8_ASCII_STRING(isolate, "ArangoError"));

  // ArangoError is a "sub-class" of Error
  v8::Handle<v8::Function> ArangoErrorFunc = ft->GetFunction(TRI_IGETC).FromMaybe(v8::Handle<v8::Function>());
  v8::Handle<v8::Value> ErrorObject =
    context->Global()->Get(context, TRI_V8_ASCII_STRING(isolate, "Error")).FromMaybe(v8::Handle<v8::Value>());
  v8::Handle<v8::Value> ErrorPrototype =
      ErrorObject->ToObject(context)
          .FromMaybe(v8::Local<v8::Object>())
    ->Get(context, TRI_V8_ASCII_STRING(isolate, "prototype")).FromMaybe(v8::Handle<v8::Value>());

  ArangoErrorFunc->Get(context,
                       TRI_V8_ASCII_STRING(isolate, "prototype")).FromMaybe(v8::Local<v8::Value>())
      ->ToObject(context)
      .FromMaybe(v8::Local<v8::Object>())
      ->SetPrototype(context, ErrorPrototype)
      .FromMaybe(false);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "ArangoError"), ArangoErrorFunc);

  rt = ft->InstanceTemplate();
  v8g->ArangoErrorTempl.Reset(isolate, rt);

  // .............................................................................
  // create the global functions
  // .............................................................................

  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "FS_CHMOD"), JS_ChMod);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_EXISTS"), JS_Exists);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_FILESIZE"), JS_SizeFile);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_GET_TEMP_FILE"),
                               JS_GetTempFile);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_GET_TEMP_PATH"),
                               JS_GetTempPath);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_IS_DIRECTORY"),
                               JS_IsDirectory);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_IS_FILE"), JS_IsFile);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "FS_LIST"), JS_List);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_LIST_TREE"), JS_ListTree);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_MAKE_ABSOLUTE"),
                               JS_MakeAbsolute);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "FS_MAKE_DIRECTORY"), JS_MakeDirectory);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "FS_MAKE_DIRECTORY_RECURSIVE"),
      JS_MakeDirectoryRecursive);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "FS_MOVE"), JS_MoveFile);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "FS_COPY_RECURSIVE"), JS_CopyRecursive);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_COPY_FILE"), JS_CopyFile);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_LINK_FILE"), JS_LinkFile);

  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "FS_MTIME"), JS_MTime);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_REMOVE"), JS_Remove);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "FS_REMOVE_DIRECTORY"),
                               JS_RemoveDirectory);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "FS_REMOVE_RECURSIVE_DIRECTORY"),
      JS_RemoveRecursiveDirectory);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_UNZIP_FILE"), JS_UnzipFile);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_ZIP_FILE"), JS_ZipFile);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "FS_ADLER32"), JS_Adler32);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_APPEND"), JS_Append);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_BASE64DECODE"),
                               JS_Base64Decode);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_BASE64ENCODE"),
                               JS_Base64Encode);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_CHECK_AND_MARK_NONCE"), JS_MarkNonce);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_CREATE_NONCE"),
                               JS_CreateNonce);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_DOWNLOAD"), JS_Download);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_EXECUTE"), JS_Execute);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_EXECUTE_EXTERNAL"),
                               JS_ExecuteExternal);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_EXECUTE_EXTERNAL_AND_WAIT"),
      JS_ExecuteExternalAndWait);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_GEN_RANDOM_ALPHA_NUMBERS"), JS_RandomAlphaNum);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_GEN_RANDOM_NUMBERS"),
                               JS_RandomNumbers);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_GEN_RANDOM_SALT"), JS_RandomSalt);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_GETLINE"), JS_Getline);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_HMAC"), JS_HMAC);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_IS_IP"), JS_IsIP);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_SPLIT_WORDS_ICU"), JS_SplitWordlist, true);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_PROCESS_STATISTICS_EXTERNAL"),
                               JS_ProcessStatisticsExternal);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_GET_EXTERNAL_SPAWNED"),
                               JS_GetExternalSpawned);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_KILL_EXTERNAL"), JS_KillExternal);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_SUSPEND_EXTERNAL"),
                               JS_SuspendExternal);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_CONTINUE_EXTERNAL"),
                               JS_ContinueExternal);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_STATUS_EXTERNAL"), JS_StatusExternal);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_LOAD"), JS_Load);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_LOG"), JS_Log);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_LOG_LEVEL"), JS_LogLevel);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_MD5"), JS_Md5);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_OPTIONS"), JS_Options);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_OUTPUT"), JS_Output);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_POLLSTDIN"), JS_PollStdin);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_PARSE"), JS_Parse);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_PARSE_FILE"), JS_ParseFile);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_PBKDF2HS1"), JS_PBKDF2HS1);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_PBKDF2"), JS_PBKDF2);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_PROCESS_STATISTICS"),
                               JS_ProcessStatisticsSelf);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_GET_PID"), JS_GetPid);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_RAND"), JS_Rand);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_READ"), JS_Read);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_READPIPE"), JS_ReadPipe);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_WRITEPIPE"), JS_WritePipe);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_CLOSEPIPE"), JS_ClosePipe);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_READ64"), JS_Read64);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_READ_BUFFER"),
                               JS_ReadBuffer);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_READ_GZIP"),
                               JS_ReadGzip);
#ifdef USE_ENTERPRISE
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_READ_DECRYPT"),
                               JS_ReadDecrypt);
#endif
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_SHA1"), JS_Sha1);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_RSAPRIVSIGN"), JS_RsaPrivSign);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_SHA224"), JS_Sha224);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_SHA256"), JS_Sha256);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_SHA384"), JS_Sha384);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_SHA512"), JS_Sha512);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_SLEEP"), JS_Sleep);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_SPRINTF"), JS_SPrintF);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_TEST_PORT"), JS_TestPort);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_TIME"), JS_Time);
  TRI_AddGlobalFunctionVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "SYS_WAIT"), JS_Wait);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_WRITE"), JS_Write);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_DEBUG_CAN_USE_FAILAT"),
                               JS_DebugCanUseFailAt);
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_IS_STOPPING"),
                               JS_IsStopping);
  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_TERMINAL_SIZE"), JS_termsize);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "V8_TO_VPACK"), JS_V8ToVPack);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "VPACK_TO_V8"), JS_VPackToV8);

  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_IS_ENTERPRISE"), JS_IsEnterprise);

  TRI_AddGlobalFunctionVocbase(
      isolate, TRI_V8_ASCII_STRING(isolate, "SYS_ERROR_NUMBER_TO_HTTP_CODE"), JS_ErrorNumberToHttpCode);

  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "IS_ALLOWED_DATABASE_NAME"), JS_IsAllowedDatabaseName);
  // .............................................................................
  // create the global variables
  // .............................................................................

  TRI_AddGlobalVariableVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "HOME"),
                               TRI_V8_STD_STRING(isolate, FileUtils::homeDirectory()));

  TRI_AddGlobalVariableVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "MODULES_PATH"),
                               V8PathList(isolate, modules));

  TRI_AddGlobalVariableVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "STARTUP_PATH"),
                               TRI_V8_STD_STRING(isolate, startupPath));

  TRI_AddGlobalVariableVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "PATH_SEPARATOR"),
                               TRI_V8_ASCII_STRING(isolate, TRI_DIR_SEPARATOR_STR));

#ifdef COVERAGE
  TRI_AddGlobalVariableVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "COVERAGE"),
                               v8::True(isolate));
#else
  TRI_AddGlobalVariableVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "COVERAGE"),
                               v8::False(isolate));
#endif

  TRI_AddGlobalVariableVocbase(isolate, TRI_V8_ASCII_STRING(isolate, "VERSION"),
                               TRI_V8_ASCII_STRING(isolate, ARANGODB_VERSION));

  TRI_AddGlobalVariableVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate, "SYS_PLATFORM"),
                               TRI_V8_ASCII_STRING(isolate, TRI_PLATFORM));

  TRI_InitV8Env(isolate, context);
}
