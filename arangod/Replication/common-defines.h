////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdlib>
#include <string>

#include "Basics/Common.h"

namespace arangodb {

/// @brief replication operations
enum TRI_replication_operation_e {
  REPLICATION_INVALID = 0,

  // REPLICATION_STOP = 1000,   // not used in ArangoDB 2.2 and higher
  // REPLICATION_START = 1001,  // not used in ArangoDB 2.2 and higher

  REPLICATION_DATABASE_CREATE = 1100,
  REPLICATION_DATABASE_DROP = 1101,

  REPLICATION_COLLECTION_CREATE = 2000,
  REPLICATION_COLLECTION_DROP = 2001,
  REPLICATION_COLLECTION_RENAME = 2002,
  REPLICATION_COLLECTION_CHANGE = 2003,
  REPLICATION_COLLECTION_TRUNCATE = 2004,

  REPLICATION_INDEX_CREATE = 2100,
  REPLICATION_INDEX_DROP = 2101,
  REPLICATION_INDEX_CHANGE = 2102,

  REPLICATION_VIEW_CREATE = 2110,
  REPLICATION_VIEW_DROP = 2111,
  REPLICATION_VIEW_CHANGE = 2112,

  REPLICATION_TRANSACTION_START = 2200,
  REPLICATION_TRANSACTION_COMMIT = 2201,
  REPLICATION_TRANSACTION_ABORT = 2202,

  REPLICATION_MARKER_DOCUMENT = 2300,
  // REPLICATION_MARKER_EDGE = 2301, // not used anymore since 3.0
  REPLICATION_MARKER_REMOVE = 2302,

  REPLICATION_MAX
};

/// @brief generate a timestamp string in a target buffer
void TRI_GetTimeStampReplication(char*, size_t);

/// @brief generate a timestamp string in a target buffer
void TRI_GetTimeStampReplication(double, char*, size_t);

/// @brief determine whether a collection should be included in replication
bool TRI_ExcludeCollectionReplication(std::string const& name,
                                      bool includeSystem,
                                      bool includeFoxxQueues);

}  // namespace arangodb
