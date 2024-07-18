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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Basics/NumberOfCores.h"
#include "Basics/StringUtils.h"
#include "Basics/operating-system.h"
#include "Basics/files.h"

#ifdef TRI_HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <cstdint>
#include <string>
#include <thread>

using namespace arangodb;

#ifndef USE_ENTERPRISE

namespace {

std::size_t numberOfCoresImpl() {
#ifdef TRI_SC_NPROCESSORS_ONLN
  auto n = sysconf(_SC_NPROCESSORS_ONLN);

  if (n < 0) {
    n = 0;
  }

  if (n > 0) {
    return static_cast<std::size_t>(n);
  }
#endif

  return static_cast<std::size_t>(std::thread::hardware_concurrency());
}

struct NumberOfCoresCache {
  NumberOfCoresCache() : cachedValue(numberOfCoresImpl()), overridden(false) {
    std::string value;
    if (TRI_GETENV("ARANGODB_OVERRIDE_DETECTED_NUMBER_OF_CORES", value)) {
      if (!value.empty()) {
        uint64_t v = arangodb::basics::StringUtils::uint64(value);
        if (v != 0) {
          cachedValue = static_cast<std::size_t>(v);
          overridden = true;
        }
      }
    }
  }

  std::size_t cachedValue;
  bool overridden;
};

NumberOfCoresCache const cache;

}  // namespace

/// @brief return number of cores from cache
std::size_t arangodb::NumberOfCores::getValue() { return ::cache.cachedValue; }

/// @brief return if number of cores was overridden
bool arangodb::NumberOfCores::overridden() { return ::cache.overridden; }

#endif
