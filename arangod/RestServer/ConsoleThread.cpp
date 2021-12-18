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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ConsoleThread.h"

#include <v8.h>
#include <iostream>

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/MutexLocker.h"
#include "Basics/application-exit.h"
#include "Basics/tri-strings.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "Rest/Version.h"
#include "V8/JavaScriptSecurityContext.h"
#include "V8/V8LineEditor.h"
#include "V8/v8-conv.h"
#include "V8/v8-utils.h"
#include "V8Server/V8DealerFeature.h"
#include "VocBase/vocbase.h"

#ifdef TRI_HAVE_SIGNAL_H
#include <signal.h>
#endif

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::rest;

V8LineEditor* ConsoleThread::serverConsole = nullptr;
Mutex ConsoleThread::serverConsoleMutex;

ConsoleThread::ConsoleThread(ApplicationServer& applicationServer, TRI_vocbase_t* vocbase)
    : Thread(applicationServer, "Console"), _vocbase(vocbase), _userAborted(false) {}

ConsoleThread::~ConsoleThread() { shutdown(); }

static char const* USER_ABORTED = "user aborted";

void ConsoleThread::run() {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  bool v8Enabled = _server.hasFeature<V8DealerFeature>() &&
                   _server.isEnabled<V8DealerFeature>() &&
                   _server.getFeature<V8DealerFeature>().isEnabled();
  if (!v8Enabled) {
    LOG_TOPIC("4a00f", FATAL, arangodb::Logger::FIXME) << "V8 engine is not enabled";
    FATAL_ERROR_EXIT();
  }

  // enter V8 context
  JavaScriptSecurityContext securityContext = JavaScriptSecurityContext::createAdminScriptContext();
  V8ContextGuard guard(_vocbase, securityContext);

  // work
  try {
    inner(guard);
  } catch (char const* error) {
    if (strcmp(error, USER_ABORTED) != 0) {
      LOG_TOPIC("6e7fd", ERR, arangodb::Logger::FIXME) << error;
    }
  } catch (...) {
    _server.beginShutdown();
    throw;
  }

  // exit context
  _server.beginShutdown();
}

void ConsoleThread::inner(V8ContextGuard const& guard) {
  // flush all log output before we print the console prompt
  Logger::flush();

  v8::Isolate* isolate = guard.isolate();
  v8::HandleScope globalScope(isolate);

  // run the shell
  std::cout << "arangod console (" << rest::Version::getVerboseVersionString()
            << ")" << std::endl;
  std::cout << "Copyright (c) ArangoDB GmbH" << std::endl;

  v8::Local<v8::String> name(TRI_V8_ASCII_STRING(isolate, TRI_V8_SHELL_COMMAND_NAME));

  auto localContext = v8::Local<v8::Context>::New(isolate, guard.context()->_context);
  localContext->Enter();
  {
    v8::Context::Scope contextScope(localContext);

    // .............................................................................
    // run console
    // .............................................................................

    uint64_t const gcInterval = 10;
    uint64_t nrCommands = 0;

    // read and eval .arangod.rc from home directory if it exists
    char const* startupScript = R"SCRIPT(
start_pretty_print(true);
start_color_print('arangodb', true);

(function () {
  var __fs__ = require("fs");
  var __rcf__ = __fs__.join(__fs__.home(), ".arangod.rc");
  if (__fs__.exists(__rcf__)) {
    try {
      var __content__ = __fs__.read(__rcf__);
      eval(__content__);
    }
    catch (err) {
      require("console").log("error in rc file '%s': %s", __rcf__, String(err.stack || err));
    }
  }
})();
)SCRIPT";

    TRI_ExecuteJavaScriptString(isolate, localContext,
                                TRI_V8_ASCII_STRING(isolate, startupScript),
                                TRI_V8_ASCII_STRING(isolate, "(startup)"), false);

#ifndef _WIN32
    // allow SIGINT in this particular thread... otherwise we cannot CTRL-C the
    // console
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);

    if (pthread_sigmask(SIG_UNBLOCK, &set, nullptr) < 0) {
      LOG_TOPIC("62022", ERR, arangodb::Logger::FIXME)
          << "unable to install signal handler";
    }
#endif

    V8LineEditor console(isolate, localContext, ".arangod.history");

    console.open(true);

    {
      MUTEX_LOCKER(mutexLocker, serverConsoleMutex);
      serverConsole = &console;
    }

    bool lastEmpty = false;

    while (!isStopping() && !_userAborted.load()) {
      if (nrCommands >= gcInterval || V8PlatformFeature::isOutOfMemory(isolate)) {
        TRI_RunGarbageCollectionV8(isolate, 0.5);
        nrCommands = 0;

        // needs to be reset after the garbage collection
        V8PlatformFeature::resetOutOfMemory(isolate);
      }

      std::string input;
      ShellBase::EofType eof;

      isolate->CancelTerminateExecution();

      {
        MUTEX_LOCKER(mutexLocker, serverConsoleMutex);
        input = console.prompt("arangod> ", "arangod>", eof);
      }

      if (eof == ShellBase::EOF_FORCE_ABORT || (eof == ShellBase::EOF_ABORT && lastEmpty)) {
        _userAborted.store(true);
      }

      if (_userAborted.load()) {
        break;
      }

      if (input.empty()) {
        lastEmpty = true;
        continue;
      }
      lastEmpty = false;

      nrCommands++;
      console.addHistory(input);

      {
        v8::TryCatch tryCatch(isolate);
        v8::HandleScope scope(isolate);

        console.setExecutingCommand(true);
        TRI_ExecuteJavaScriptString(isolate, localContext,
                                    TRI_V8_STD_STRING(isolate, input), name,
                                    true, false);
        console.setExecutingCommand(false);

        if (_userAborted.load()) {
          std::cout << "command aborted" << std::endl;
        } else if (tryCatch.HasCaught()) {
          if (!tryCatch.CanContinue() || tryCatch.HasTerminated()) {
            std::cout << "command aborted" << std::endl;
          } else {
            std::cout << TRI_StringifyV8Exception(isolate, &tryCatch);
          }
        }
      }
    }

    {
      MUTEX_LOCKER(mutexLocker, serverConsoleMutex);
      serverConsole = nullptr;
    }
  }

  localContext->Exit();
  throw USER_ABORTED;
}
