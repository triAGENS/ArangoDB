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
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#include "ProduceAQLResultsState.h"

#include "Pregel/Conductor/ExecutionStates/FatalErrorState.h"
#include "Pregel/Conductor/ExecutionStates/AQLResultsAvailableState.h"
#include "Pregel/Conductor/State.h"
#include "CanceledState.h"

using namespace arangodb::pregel::actor;
using namespace arangodb::pregel::conductor;

ProduceAQLResults::ProduceAQLResults(ConductorState& conductor)
    : conductor{conductor} {}

auto ProduceAQLResults::receive(actor::ActorPID sender,
                                message::ConductorMessages message)
    -> std::optional<StateChange> {
  if (std::holds_alternative<message::Cancel>(message)) {
    auto newState = std::make_unique<Canceled>(conductor);
    auto stateName = newState->name();

    return StateChange{
        .statusMessage =
            pregel::message::Canceled{
                .state = stateName,
                .prevState = pregel::message::PrevState::COMPUTING},
        .newState = std::move(newState)};
  }

  if (not std::holds_alternative<message::ResultCreated>(message)) {
    auto newState = std::make_unique<FatalError>(conductor);
    auto stateName = newState->name();
    return StateChange{
        .statusMessage =
            pregel::message::InFatalError{
                .state = stateName,
                .prevState = pregel::message::PrevState::COMPUTING},
        .newState = std::move(newState)};
  }
  responseCount++;
  respondedWorkers.emplace(sender);

  if (responseCount == conductor.workers.size() and
      respondedWorkers == conductor.workers) {
    auto newState = std::make_unique<AQLResultsAvailable>(conductor);
    auto stateName = newState->name();
    return StateChange{
        .statusMessage = pregel::message::PregelFinished{.state = stateName},
        .newState = std::move(newState)};
  }
  return std::nullopt;
}
