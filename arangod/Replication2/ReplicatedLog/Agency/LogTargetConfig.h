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

namespace arangodb::replication2::agency {

struct LogTargetConfig {
  std::size_t writeConcern = 1;
  std::size_t softWriteConcern = 1;
  bool waitForSync = false;

  LogTargetConfig() noexcept = default;
  LogTargetConfig(std::size_t writeConcern, std::size_t softWriteConcern,
                  bool waitForSync) noexcept
      : writeConcern(writeConcern),
        softWriteConcern(softWriteConcern),
        waitForSync(waitForSync) {}

  friend auto operator==(LogTargetConfig const& left,
                         LogTargetConfig const& right) noexcept
      -> bool = default;
};

template<class Inspector>
auto inspect(Inspector& f, LogTargetConfig& x) {
  return f.object(x).fields(f.field("writeConcern", x.writeConcern),
                            f.field("softWriteConcern", x.softWriteConcern)
                                .fallback(std::ref(x.writeConcern)),
                            f.field("waitForSync", x.waitForSync));
}

}  // namespace arangodb::replication2::agency
