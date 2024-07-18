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

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace arangodb {

class alignas(64) GlobalResourceMonitor final {
  GlobalResourceMonitor(GlobalResourceMonitor const&) = delete;
  GlobalResourceMonitor& operator=(GlobalResourceMonitor const&) = delete;

 public:
  constexpr GlobalResourceMonitor()
      : _current(0),
        _limit(0),
        _globalLimitReachedCounter(0),
        _localLimitReachedCounter(0) {}

  struct Stats {
    std::uint64_t globalLimitReached;
    std::uint64_t localLimitReached;
  };

  /// @brief set the global memory limit
  void memoryLimit(std::int64_t value) noexcept;

  /// @brief return the global memory limit
  std::int64_t memoryLimit() const noexcept;

  /// @brief return the current global memory usage
  std::int64_t current() const noexcept;

  /// @brief number of times the global and any local limits were reached
  Stats stats() const noexcept;

  /// @brief increase the counter for global memory limit violations
  void trackGlobalViolation() noexcept;

  /// @brief increase the counter for local memory limit violations
  void trackLocalViolation() noexcept;

  /// @brief increase global memory usage by <value> bytes. if increasing
  /// exceeds the memory limit, does not perform the increase and returns false.
  /// if increasing succeeds, the global value is modified and true is returned
  /// Note: value must be >= 0
  [[nodiscard]] bool increaseMemoryUsage(std::int64_t value) noexcept;

  /// @brief decrease current global memory usage by <value> bytes. will not
  /// throw Note: value must be >= 0
  void decreaseMemoryUsage(std::int64_t value) noexcept;

  /// @brief Unconditionally updates the current memory usage with the given
  /// value. Since the parameter is signed, this method can increase or
  /// decrease!
  void forceUpdateMemoryUsage(std::int64_t value) noexcept;

  /// @brief returns a reference to a global shared instance
  static GlobalResourceMonitor& instance() noexcept;

 private:
  /// @brief the current combined memory usage of all tracked operations.
  /// Theoretically it can happen that the global limit is exceeded due to the
  /// correction applied as part of the rollback in increaseMemoryUsage, but at
  /// least this excess is bounded. This counter is updated by local instances
  /// only if there is a substantial allocation/deallocation. it is
  /// intentionally _not_ updated on every small allocation/deallocation. the
  /// granularity for the values in this counter is chunkSize.
  std::atomic<std::int64_t> _current;

  /// @brief maximum allowed global memory limit for all tracked operations
  /// combined. a value of 0 means that there will be no global limit enforced.
  std::int64_t _limit;

  /// @brief number of times the global memory limit was reached
  std::atomic<std::uint64_t> _globalLimitReachedCounter;

  /// @brief number of times a local memory limit was reached
  std::atomic<std::uint64_t> _localLimitReachedCounter;
};

}  // namespace arangodb
