////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
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
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>

namespace arangodb::replication2 {

// These settings are initialised by the ReplicatedLogFeature based on command
// line arguments
struct ReplicatedLogGlobalSettings {
 public:
  static inline constexpr std::size_t defaultThresholdNetworkBatchSize{1024 *
                                                                       1024};
  static inline constexpr std::size_t minThresholdNetworkBatchSize{1024 * 1024};

  static inline constexpr std::size_t defaultThresholdRocksDBWriteBatchSize{
      1024 * 1024};
  static inline constexpr std::size_t minThresholdRocksDBWriteBatchSize{1024 *
                                                                        1024};
  static inline constexpr std::size_t defaultThresholdLogCompaction{1000};

  std::size_t _thresholdNetworkBatchSize{defaultThresholdNetworkBatchSize};
  std::size_t _thresholdRocksDBWriteBatchSize{
      defaultThresholdRocksDBWriteBatchSize};
  std::size_t _thresholdLogCompaction{defaultThresholdLogCompaction};
};

}  // namespace arangodb::replication2
