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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////
#include "SupervisionAction.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-common.h>

#include "Inspection/VPack.h"
#include "Logger/LogMacros.h"
#include "Replication2/AgencyCollectionSpecification.h"
#include "Replication2/ReplicatedLog/AgencyLogSpecification.h"
#include "Replication2/ReplicatedLog/AgencySpecificationInspectors.h"

#include <fmt/core.h>
#include <variant>

using namespace arangodb::replication2::agency;

namespace arangodb::replication2::replicated_log {

auto executeAction(Log log, Action& action) -> ActionContext {
  auto currentSupervision =
      std::invoke([&]() -> std::optional<LogCurrentSupervision> {
        if (log.current.has_value()) {
          return log.current->supervision;
        } else {
          return std::nullopt;
        }
      });

  if (!currentSupervision.has_value()) {
    currentSupervision.emplace();
  }

  auto ctx = ActionContext{std::move(log.plan), std::move(currentSupervision)};

  std::visit([&](auto&& action) { action.execute(ctx); }, action);
  return ctx;
}

}  // namespace arangodb::replication2::replicated_log
