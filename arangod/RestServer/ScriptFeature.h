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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef USE_V8
#error this file is not supposed to be used in builds with -DUSE_V8=Off
#endif

#include "RestServer/arangod.h"
#include "GeneralServer/OperationMode.h"

namespace arangodb {

class ScriptFeature final : public ArangodFeature {
 public:
  static constexpr std::string_view name() noexcept { return "Script"; }

  explicit ScriptFeature(ArangodServer& server, int* result);

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void start() override final;

 private:
  std::vector<std::string> _scriptParameters;

  int runScript(std::vector<std::string> const& scripts);

  int* _result;
};

}  // namespace arangodb
