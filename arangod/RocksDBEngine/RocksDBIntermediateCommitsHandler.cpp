////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
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

#include "RocksDBIntermediateCommitsHandler.h"
#include "RocksDBEngine/Methods/RocksDBTrxMethods.h"
#include "RocksDBEngine/RocksDBTransactionState.h"

namespace arangodb {
   
// helper class to delay intermediate commits if required. 
// this is useful to run a full array of insert/update/replace/remove
// operations without an interruption in the middle by an intermediate
// commit. this is especially useful for synchronous replication, where
// we do not want to make an intermediate commit halfway into an array
// of operations on the leader, because the intermediate commit will
// unlock all previously locked keys in RocksDB. 
// for synchronous replication, we want to keep the locks on the keys
// until the operations have been replication to followers, because the
// key locks on the leader serialize not only the operations on the
// leader but also the operations replicated to followers.
RocksDBIntermediateCommitsHandler::RocksDBIntermediateCommitsHandler(transaction::Methods* trx, DataSourceId id)
    : IntermediateCommitsHandler(trx, id) {}

Result RocksDBIntermediateCommitsHandler::commit() {
  bool hasPerformedIntermediateCommit = false;
  return static_cast<RocksDBTransactionState*>(_trx->state())->rocksdbMethods(_id)->checkIntermediateCommit(hasPerformedIntermediateCommit);
}

}  // namespace arangodb
