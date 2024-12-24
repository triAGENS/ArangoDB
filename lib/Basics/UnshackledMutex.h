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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <absl/synchronization/mutex.h>

namespace arangodb::basics {

// @brief Normal Mutexes, like std::mutex or pthread_mutex_t, require to be
// unlocked by the same thread they were locked. However, with rising use of
// async programming / futures, this is not always practical anymore.
// This mutex lifts this requirement and can be unlocked from another thread.
class UnshackledMutex {
 public:
  void lock() noexcept;
  void unlock() noexcept;
  bool try_lock() noexcept;

 private:
  absl::Mutex _mutex;
  bool _locked{false};
};

}  // namespace arangodb::basics
