////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#include "ModelChecker.h"

using namespace arangodb::test;
using namespace arangodb::test::model_checker;

auto model_checker::operator<<(std::ostream& os, Stats const& stats)
    -> std::ostream& {
  return os << "unique: " << stats.uniqueStates
            << " eliminated: " << stats.eliminatedStates
            << " discovered: " << stats.discoveredStates
            << " final: " << stats.finalStates
            << " dismissed: " << stats.dismissedStates;
}
