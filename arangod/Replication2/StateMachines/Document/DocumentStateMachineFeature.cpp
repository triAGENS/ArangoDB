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
/// @author Alexandru Petenchea
////////////////////////////////////////////////////////////////////////////////

#include "Replication2/ReplicatedState/ReplicatedStateFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ServerState.h"
#include "Cluster/MaintenanceFeature.h"
#include "Logger/LogContextKeys.h"
#include "Network/NetworkFeature.h"
#include "Transaction/Manager.h"
#include "Transaction/ManagerFeature.h"
#include "RestServer/DatabaseFeature.h"

#include "Replication2/StateMachines/Document/DocumentFollowerState.h"
#include "Replication2/StateMachines/Document/DocumentLeaderState.h"
#include "Replication2/StateMachines/Document/DocumentStateMachineFeature.h"
#include "Replication2/StateMachines/Document/DocumentStateMachine.h"
#include "Replication2/StateMachines/Document/DocumentStateHandlersFactory.h"

using namespace arangodb::replication2::replicated_state::document;
void DocumentStateMachineFeature::prepare() {
  bool const enabled = ServerState::instance()->isDBServer();
  setEnabled(enabled);
}

void DocumentStateMachineFeature::start() {
  ArangodServer& s = server();
  auto& replicatedStateFeature = s.getFeature<ReplicatedStateAppFeature>();
  auto& networkFeature = s.getFeature<NetworkFeature>();
  auto& maintenanceFeature = s.getFeature<MaintenanceFeature>();
  auto defaultLoggerContext =
      LoggerContext(Logger::REPLICATED_STATE)
          .with<logContextKeyStateImpl>(DocumentState::NAME);

  replicatedStateFeature.registerStateType<DocumentState>(
      std::string{DocumentState::NAME},
      std::make_shared<DocumentStateHandlersFactory>(
          networkFeature.pool(), maintenanceFeature,
          std::move(defaultLoggerContext)),
      *transaction::ManagerFeature::manager());
}

DocumentStateMachineFeature::DocumentStateMachineFeature(Server& server)
    : ArangodFeature{server, *this} {
  setOptional(true);
  startsAfter<ClusterFeature>();
  startsAfter<NetworkFeature>();
  startsAfter<MaintenanceFeature>();
  startsAfter<ReplicatedStateAppFeature>();
  onlyEnabledWith<ClusterFeature>();
  onlyEnabledWith<ReplicatedStateAppFeature>();
}
