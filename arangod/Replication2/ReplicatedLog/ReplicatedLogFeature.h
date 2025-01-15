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

#include "Replication2/ReplicatedLog/LogCommon.h"
#include "RestServer/arangod.h"

#include <cstdint>

namespace arangodb::replication2::replicated_log {
struct ReplicatedLogMetrics;
}

namespace arangodb {
class ReplicatedLogFeature final : public ArangodFeature {
 public:
  static constexpr std::string_view name() noexcept { return "ReplicatedLog"; }

  explicit ReplicatedLogFeature(Server& server);
  ~ReplicatedLogFeature() override;

  void start() override;

  auto metrics() const noexcept -> std::shared_ptr<
      replication2::replicated_log::ReplicatedLogMetrics> const&;
  auto options() const noexcept
      -> std::shared_ptr<replication2::ReplicatedLogGlobalSettings const>;

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override;

 private:
  std::shared_ptr<replication2::replicated_log::ReplicatedLogMetrics>
      _replicatedLogMetrics;
  std::shared_ptr<replication2::ReplicatedLogGlobalSettings> _options;
};

}  // namespace arangodb
