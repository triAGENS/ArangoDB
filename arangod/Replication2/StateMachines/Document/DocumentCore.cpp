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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#include "DocumentCore.h"
#include "DocumentStateMachine.h"

using namespace arangodb::replication2::replicated_state::document;

DocumentCore::DocumentCore(
    GlobalLogIdentifier gid, DocumentCoreParameters coreParameters,
    std::shared_ptr<IDocumentStateAgencyReader> agencyReader,
    std::shared_ptr<IDocumentStateShardHandler> shardHandler,
    LoggerContext loggerContext)
    : loggerContext(std::move(loggerContext)),
      _gid(std::move(gid)),
      _params(std::move(coreParameters)),
      _agencyReader(std::move(agencyReader)),
      _shardHandler(std::move(shardHandler)) {
  auto collectionProperties =
      _agencyReader->getCollectionInfo(_gid.database, _params.collectionId);
  auto result = _shardHandler->createShard(_gid, _params.collectionId,
                                           std::move(collectionProperties));
  TRI_ASSERT(result.ok());
  LOG_CTX("b7e0d", TRACE, loggerContext) << "Created shard " << result.get();
}
