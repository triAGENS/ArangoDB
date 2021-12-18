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

#include <string>

#include <v8.h>

#include "Basics/Common.h"
#include "Utilities/ScriptLoader.h"

namespace arangodb {
namespace velocypack {
class Builder;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief JavaScript source code loader
////////////////////////////////////////////////////////////////////////////////

class JSLoader : public ScriptLoader {
 public:
  enum eState { eFailLoad, eFailExecute, eSuccess };

  //////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a loader
  //////////////////////////////////////////////////////////////////////////////

  JSLoader();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief loads a named script, if the builder pointer is not nullptr the
  /// returned result will be written there as vpack
  //////////////////////////////////////////////////////////////////////////////

  JSLoader::eState loadScript(v8::Isolate* isolate,
                              v8::Handle<v8::Context>&,
                              std::string const& name,
                              velocypack::Builder* builder,
                              bool logStacktraceError = false);
};
}  // namespace arangodb

