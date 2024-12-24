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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "RequestLane.h"

#include "Basics/debugging.h"
#include <iostream>

namespace {
std::string LaneName(arangodb::RequestLane lane) {
  switch (lane) {
    case arangodb::RequestLane::CLIENT_FAST:
      return "CLIENT_FAST";
    case arangodb::RequestLane::CLIENT_AQL:
      return "CLIENT_AQL";
    case arangodb::RequestLane::CLIENT_V8:
      return "CLIENT_V8";
    case arangodb::RequestLane::CLIENT_SLOW:
      return "CLIENT_SLOW";
    case arangodb::RequestLane::CLIENT_UI:
      return "CLIENT_UI";
    case arangodb::RequestLane::AGENCY_INTERNAL:
      return "AGENCY_INTERNAL";
    case arangodb::RequestLane::AGENCY_CLUSTER:
      return "AGENCY_CLUSTER";
    case arangodb::RequestLane::CLUSTER_INTERNAL:
      return "CLUSTER_INTERNAL";
    case arangodb::RequestLane::CLUSTER_AQL:
      return "CLUSTER_AQL";
    case arangodb::RequestLane::CLUSTER_AQL_INTERNAL_COORDINATOR:
      return "CLUSTER_AQL_INTERNAL_COORDINATOR";
    case arangodb::RequestLane::CLUSTER_AQL_SHUTDOWN:
      return "CLUSTER_AQL_SHUTDOWN";
    case arangodb::RequestLane::CLUSTER_AQL_DOCUMENT:
      return "CLUSTER_AQL_DOCUMENT";
    case arangodb::RequestLane::CLUSTER_V8:
      return "CLUSTER_V8";
    case arangodb::RequestLane::CLUSTER_ADMIN:
      return "CLUSTER_ADMIN";
    case arangodb::RequestLane::SERVER_REPLICATION:
      return "SERVER_REPLICATION";
    case arangodb::RequestLane::SERVER_REPLICATION_CATCHUP:
      return "SERVER_REPLICATION_CATCHUP";
    case arangodb::RequestLane::SERVER_SYNCHRONOUS_REPLICATION:
      return "SERVER_SYNCHRONOUS_REPLICATION";
    case arangodb::RequestLane::TASK_V8:
      return "TASK_V8";
    case arangodb::RequestLane::INTERNAL_LOW:
      return "INTERNAL_LOW";
    case arangodb::RequestLane::CONTINUATION:
      return "CONTINUATION";
    case arangodb::RequestLane::DELAYED_FUTURE:
      return "DELAYED_FUTURE";
    case arangodb::RequestLane::UNDEFINED:
      return "UNDEFINED";
  }
  // Should not be reachable
  TRI_ASSERT(false);
  return "UNDEFINED";
}

std::string PriorityName(arangodb::RequestLane lane) {
  auto prio = arangodb::PriorityRequestLane(lane);

  switch (prio) {
    case arangodb::RequestPriority::MAINTENANCE:
      return "MAINTENANCE";
    case arangodb::RequestPriority::HIGH:
      return "HIGH";
    case arangodb::RequestPriority::MED:
      return "MED";
    case arangodb::RequestPriority::LOW:
      return "LOW";
  }
  // Should not be reachable
  TRI_ASSERT(false);
  return "UNDEFINED";
}
}  // namespace

namespace arangodb {
std::ostream& operator<<(std::ostream& out, RequestLane const& lane) {
  out << LaneName(lane);
  out << " with priority: ";
  out << PriorityName(lane);
  return out;
}

std::string to_string(RequestLane lane) {
  auto ss = std::stringstream();
  ss << lane;
  return std::move(ss).str();
}

}  // namespace arangodb
