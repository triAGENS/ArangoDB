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

#include <iosfwd>

namespace arangodb {
enum class RequestLane {
  // For requests that do not block or wait for something.
  // This ignores blocks that can occur when delivering
  // a file from, for example, an NFS mount.
  CLIENT_FAST,

  // For requests that execute an AQL query or are tightly
  // related like simple queries, but not V8 actions,
  // that do AQL requests, user administrator that
  // internally uses AQL.
  CLIENT_AQL,

  // For requests that are executed within a V8 context,
  // but not for requests that might use a V8 context for
  // user defined functions.
  CLIENT_V8,

  // For requests that might block or wait for something,
  // which are not CLIENT_AQL or CLIENT_V8.
  CLIENT_SLOW,

  // Used for all requests sent by the web interface
  CLIENT_UI,

  // For requests between agents. These are basically the
  // requests used to implement RAFT.
  AGENCY_INTERNAL,

  // For requests from the DBserver or Coordinator accessing
  // the agency.
  AGENCY_CLUSTER,

  // For requests from the DBserver to the Coordinator or
  // from the Coordinator to the DBserver. But not using
  // V8 or having high priority.
  CLUSTER_INTERNAL,

  // Internal AQL requests, or continuations. Medium priority.
  CLUSTER_AQL,

  // For requests from the DBserver to the Coordinator, and continuations on the
  // Coordinator.
  // These have medium priority. Because client requests made against the
  // RestCursorHandler (with lane CLIENT_AQL) might block and need these to
  // finish. Ongoing low priority requests can also prevent low priority lanes
  // from being worked on, having the same effect.
  CLUSTER_AQL_INTERNAL_COORDINATOR,

  // Shutdown request for AQL queries, i.e. /_api/aql/finish/<id> on the
  // DBserver. This calls have slightly higher priority than normal AQL requests
  // because the query shutdown can release resources and unblock other
  // operations.
  CLUSTER_AQL_SHUTDOWN,

  // DOCUMENT() requests inside cluster AQL queries, executed on DBservers.
  // These requests will read a locally available document and do not depend
  // on other requests. They can always make progress. They will be initiated
  // on coordinators and handling them quickly may unblock the coordinator
  // part of an AQL query.
  CLUSTER_AQL_DOCUMENT,

  // For requests from the from the Coordinator to the
  // DBserver using V8.
  CLUSTER_V8,

  // For requests from the DBserver to the Coordinator or
  // from the Coordinator to the DBserver for administration
  // or diagnostic purpose. Should not block.
  CLUSTER_ADMIN,

  // For requests used between leader and follower for
  // replication to compare the local states of data.
  SERVER_REPLICATION,

  // For requests used between leader and follower for
  // replication to go the final mile and get back to
  // in-sync mode (wal tailing)
  SERVER_REPLICATION_CATCHUP,

  // For synchronous replication requests on the follower
  SERVER_SYNCHRONOUS_REPLICATION,

  // For periodic or one-off V8-based tasks executed by the
  // Scheduler.
  TASK_V8,

  // Internal tasks with low priority
  INTERNAL_LOW,

  // Default continuation lane for requests (e.g. after returning from a network
  // call). Some requests, such as CLUSTER_AQL, will have a different
  // continuation lane for more fine-grained control.
  CONTINUATION,

  // Not yet used:
  // For requests which go from the agency back to coordinators or
  // DBservers to report about changes in the agency. They are fast
  // and should have high prio. Will never block.
  // AGENCY_CALLBACK`

  // Used by futures that have been delayed using Scheduler::delay.
  DELAYED_FUTURE,

  // undefined request lane, used only in the beginning
  UNDEFINED,
};

enum class RequestPriority { MAINTENANCE = 0, HIGH = 1, MED = 2, LOW = 3 };

constexpr inline RequestPriority PriorityRequestLane(RequestLane lane) {
  switch (lane) {
    case RequestLane::CLIENT_FAST:
      return RequestPriority::MAINTENANCE;
    case RequestLane::CLIENT_AQL:
      return RequestPriority::LOW;
    case RequestLane::CLIENT_V8:
      return RequestPriority::LOW;
    case RequestLane::CLIENT_SLOW:
      return RequestPriority::LOW;
    case RequestLane::AGENCY_INTERNAL:
      return RequestPriority::HIGH;
    case RequestLane::AGENCY_CLUSTER:
      return RequestPriority::LOW;
    case RequestLane::CLUSTER_INTERNAL:
      return RequestPriority::HIGH;
    case RequestLane::CLUSTER_AQL:
      return RequestPriority::MED;
    case RequestLane::CLUSTER_AQL_INTERNAL_COORDINATOR:
      return RequestPriority::MED;
    case RequestLane::CLUSTER_AQL_SHUTDOWN:
      return RequestPriority::MED;
    case RequestLane::CLUSTER_AQL_DOCUMENT:
      return RequestPriority::MED;
    case RequestLane::CLUSTER_V8:
      return RequestPriority::LOW;
    case RequestLane::CLUSTER_ADMIN:
      return RequestPriority::HIGH;
    case RequestLane::SERVER_REPLICATION_CATCHUP:
      return RequestPriority::MED;
    case RequestLane::SERVER_REPLICATION:
      return RequestPriority::LOW;
    case RequestLane::TASK_V8:
      return RequestPriority::LOW;
    case RequestLane::INTERNAL_LOW:
      return RequestPriority::LOW;
    case RequestLane::CLIENT_UI:
      return RequestPriority::HIGH;
    case RequestLane::DELAYED_FUTURE:
      return RequestPriority::HIGH;
    case RequestLane::SERVER_SYNCHRONOUS_REPLICATION:
      return RequestPriority::HIGH;
    case RequestLane::CONTINUATION:
      return RequestPriority::MED;
    case RequestLane::UNDEFINED:
      // assume low priority for UNDEFINED. we should never get
      // here under normal circumstances. if we do, returning the
      // default shouldn't do any harm.
      return RequestPriority::LOW;
  }
  return RequestPriority::LOW;
}

std::ostream& operator<<(std::ostream&, arangodb::RequestLane const& lane);

std::string to_string(RequestLane);

}  // namespace arangodb
