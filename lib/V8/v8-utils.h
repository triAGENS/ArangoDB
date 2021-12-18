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

#pragma once

#include <stddef.h>
#include <cstdint>
#include <string>

#include <v8.h>

#include "Basics/ErrorCode.h"

namespace arangodb {
class Result;
}
////////////////////////////////////////////////////////////////////////////////
/// @brief Converts an object to a UTF-8-encoded and normalized character array.
////////////////////////////////////////////////////////////////////////////////

class TRI_Utf8ValueNFC {
 public:
  explicit TRI_Utf8ValueNFC(v8::Isolate* isolate, v8::Handle<v8::Value> const);

  ~TRI_Utf8ValueNFC();

  // Disallow copying and assigning.
  TRI_Utf8ValueNFC(TRI_Utf8ValueNFC const&) = delete;
  TRI_Utf8ValueNFC& operator=(TRI_Utf8ValueNFC const&) = delete;

  inline char* operator*() { return _str; }

  inline char const* operator*() const { return _str; }

  inline size_t length() const { return _length; }

  char* steal() {
    char* tmp = _str;
    _str = nullptr;
    return tmp;
  }

 private:
  char* _str;

  size_t _length;
};

/// @brief slot for a type
static int const SLOT_CLASS_TYPE = 0;

/// @brief slot for a "C++ class"
static int const SLOT_CLASS = 1;

/// @brief slot for a V8 external
static int const SLOT_EXTERNAL = 2;

////////////////////////////////////////////////////////////////////////////////
/// @brief shell command program name (may be printed in stack traces)
////////////////////////////////////////////////////////////////////////////////

#define TRI_V8_SHELL_COMMAND_NAME "<shell command>"

////////////////////////////////////////////////////////////////////////////////
/// @brief unwraps a C++ class given a v8::Object
////////////////////////////////////////////////////////////////////////////////

template <class T>
static T* TRI_UnwrapClass(v8::Handle<v8::Object> obj, int32_t type,
                          v8::Handle<v8::Context> context) {
  if (obj->InternalFieldCount() <= SLOT_CLASS) {
    return nullptr;
  }
  auto slot = obj->GetInternalField(SLOT_CLASS_TYPE);
  if (slot->Int32Value(context).ToChecked() != type) {
    return nullptr;
  }

  auto slotc = obj->GetInternalField(SLOT_CLASS);
  auto slotp = v8::Handle<v8::External>::Cast(slotc);
  auto val = slotp->Value();
  auto ret = static_cast<T*>(val);
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reports an exception
////////////////////////////////////////////////////////////////////////////////

std::string TRI_StringifyV8Exception(v8::Isolate* isolate, v8::TryCatch*);

////////////////////////////////////////////////////////////////////////////////
/// @brief prints an exception and stacktrace
////////////////////////////////////////////////////////////////////////////////

void TRI_LogV8Exception(v8::Isolate* isolate,
                        v8::TryCatch*,
                        bool logStacktraceError = false);

////////////////////////////////////////////////////////////////////////////////
/// @brief reads a file into the current context
////////////////////////////////////////////////////////////////////////////////

bool TRI_ExecuteGlobalJavaScriptFile(v8::Isolate* isolate, char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a file
////////////////////////////////////////////////////////////////////////////////

bool TRI_ParseJavaScriptFile(v8::Isolate* isolate, char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief executes a string within a V8 context, optionally print the result
////////////////////////////////////////////////////////////////////////////////

v8::Handle<v8::Value> TRI_ExecuteJavaScriptString(v8::Isolate* isolate,
                                                  v8::Handle<v8::Context> context,
                                                  v8::Handle<v8::String> const source,
                                                  v8::Handle<v8::String> const name,
                                                  bool printResult,
                                                  bool logExceptions = true,
                                                  bool logStacktraceError = false);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an error in a javascript object, based on error number only
////////////////////////////////////////////////////////////////////////////////

// void TRI_CreateErrorObject(v8::Isolate* isolate, ErrorCode errorNumber);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an error in a javascript object, based on arangodb::Result
////////////////////////////////////////////////////////////////////////////////
void TRI_CreateErrorObject(v8::Isolate* isolate, arangodb::Result const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an error in a javascript object
////////////////////////////////////////////////////////////////////////////////

void TRI_CreateErrorObject(v8::Isolate* isolate, ErrorCode errorNumber,
                           std::string_view message, bool autoPrepend);

////////////////////////////////////////////////////////////////////////////////
/// @brief normalize a v8 object
////////////////////////////////////////////////////////////////////////////////

void TRI_normalize_V8_Obj(v8::FunctionCallbackInfo<v8::Value> const& args,
                          v8::Handle<v8::Value> obj);

////////////////////////////////////////////////////////////////////////////////
/// @brief run the V8 garbage collection for at most a specifiable amount of
/// time
////////////////////////////////////////////////////////////////////////////////

bool TRI_RunGarbageCollectionV8(v8::Isolate*, double);

////////////////////////////////////////////////////////////////////////////////
/// @brief clear the instance-local cache of wrapped objects
////////////////////////////////////////////////////////////////////////////////

void TRI_ClearObjectCacheV8(v8::Isolate*);

////////////////////////////////////////////////////////////////////////////////
/// @brief stores the V8 utils function inside the global variable
////////////////////////////////////////////////////////////////////////////////

void TRI_InitV8Utils(v8::Isolate* isolate,
                     v8::Handle<v8::Context>,
                     std::string const& startupPath,
                     std::string const& modules);

void JS_Download(v8::FunctionCallbackInfo<v8::Value> const& args);

