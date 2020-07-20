////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
/// @author Copyright 2020, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "v8-foxx.h"

#include "Basics/WriteLocker.h"
#include "V8/v8-utils.h"
#include "V8Server/FoxxQueuesFeature.h"

using namespace arangodb;
using namespace arangodb::javascript;

static void JS_ExecuteFoxxLocked(v8::FunctionCallbackInfo<v8::Value> const& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate)
  v8::HandleScope scope(isolate);

  if (args.Length() != 1 || !args[0]->IsFunction()) {
    TRI_V8_THROW_EXCEPTION_USAGE("executeFoxxLocked(<function>)");
  }

  v8::Handle<v8::Function> action = v8::Local<v8::Function>::Cast(args[0]);
  if (action.IsEmpty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "cannot create function instance for executeFoxxLocked");
  }

  TRI_GET_GLOBALS();
  FoxxQueuesFeature& foxxQueuesFeature = v8g->_server.getFeature<FoxxQueuesFeature>();

  v8::Handle<v8::Object> current = isolate->GetCurrentContext()->Global();
  v8::Handle<v8::Value> callArgs[] = {v8::Null(isolate)};

  WRITE_LOCKER(writeLock, foxxQueuesFeature.fileSystemLock());
  v8::Handle<v8::Value> rv =
      action->Call(TRI_IGETC, current, 0, callArgs).FromMaybe(v8::Local<v8::Value>());

  TRI_V8_RETURN(rv);
  TRI_V8_TRY_CATCH_END
}

void arangodb::javascript::Initialize_Foxx(v8::Isolate* isolate) {
  TRI_AddGlobalFunctionVocbase(isolate,
                               TRI_V8_ASCII_STRING(isolate,
                                                   "SYS_EXECUTE_FOXX_LOCKED"),
                               JS_ExecuteFoxxLocked, true);
}
