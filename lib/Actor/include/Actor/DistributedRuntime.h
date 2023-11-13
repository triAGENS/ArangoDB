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
/// @author Julia Volmer
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include "Inspection/VPackWithErrorT.h"

#include "Actor/Actor.h"
#include "Actor/ActorList.h"
#include "Actor/ActorID.h"
#include "Actor/Assert.h"
#include "Actor/DistributedActorPID.h"
#include "Actor/IExternalDispatcher.h"
#include "Actor/IScheduler.h"

namespace arangodb::actor {

struct DistributedRuntime : std::enable_shared_from_this<DistributedRuntime> {
  DistributedRuntime() = delete;
  DistributedRuntime(DistributedRuntime const&) = delete;
  DistributedRuntime(DistributedRuntime&&) = delete;
  DistributedRuntime(ServerID myServerID, std::string runtimeID,
                     std::shared_ptr<IScheduler> scheduler,
                     std::shared_ptr<IExternalDispatcher> externalDispatcher)
      : myServerID(myServerID),
        runtimeID(runtimeID),
        scheduler(scheduler),
        externalDispatcher(externalDispatcher) {}

  using ActorPID = DistributedActorPID;

  template<typename ActorConfig>
  auto spawn(std::unique_ptr<typename ActorConfig::State> initialState,
             typename ActorConfig::Message initialMessage) -> ActorID {
    auto newId = ActorID{uniqueActorIDCounter++};

    // TODO - we do not want to pass the database name as part of the spawn
    // call. If we really need it as part of  the actor PID, we need to find a
    // better way.
    auto address =
        ActorPID{.server = myServerID, .database = "database", .id = newId};

    auto newActor = std::make_shared<Actor<DistributedRuntime, ActorConfig>>(
        address, this->shared_from_this(), std::move(initialState));
    actors.add(newId, std::move(newActor));

    // Send initial message to newly created actor
    dispatchLocally(address, address, initialMessage);

    return newId;
  }

  auto getActorIDs() -> std::vector<ActorID> { return actors.allIDs(); }

  auto contains(ActorID id) const -> bool { return actors.contains(id); }

  template<typename ActorConfig>
  auto getActorStateByID(ActorID id) const
      -> std::optional<typename ActorConfig::State> {
    auto actorBase = actors.find(id);
    if (actorBase.has_value()) {
      auto* actor = dynamic_cast<Actor<DistributedRuntime, ActorConfig>*>(
          actorBase->get());
      if (actor != nullptr) {
        return actor->getState();
      }
    }
    return std::nullopt;
  }

  auto getSerializedActorByID(ActorID id) const
      -> std::optional<velocypack::SharedSlice> {
    auto actor = actors.find(id);
    if (actor.has_value()) {
      if (actor->get() != nullptr) {
        return actor.value()->serialize();
      }
    }
    return std::nullopt;
  }

  auto receive(ActorPID sender, ActorPID receiver, velocypack::SharedSlice msg)
      -> void {
    auto actor = actors.find(receiver.id);
    if (actor.has_value()) {
      actor.value()->process(sender, msg);
    } else {
      auto error = message::ActorError<ActorPID>{
          message::ActorNotFound<ActorPID>{.actor = receiver}};
      auto payload = inspection::serializeWithErrorT(error);
      ACTOR_ASSERT(payload.ok());
      dispatch(receiver, sender, payload.get());
    }
  }

  template<typename ActorMessage>
  auto dispatch(ActorPID sender, ActorPID receiver, ActorMessage const& message)
      -> void {
    if (receiver.server == sender.server) {
      dispatchLocally(sender, receiver, message);
    } else {
      dispatchExternally(sender, receiver, message);
    }
  }

  template<typename ActorMessage>
  auto dispatchDelayed(std::chrono::seconds delay, ActorPID sender,
                       ActorPID receiver, ActorMessage const& message) -> void {
    scheduler->delay(delay, [self = this->weak_from_this(), sender, receiver,
                             message](bool canceled) {
      auto me = self.lock();
      if (me != nullptr) {
        me->dispatch(sender, receiver, message);
      }
    });
  }

  auto areAllActorsIdle() -> bool {
    return actors.checkAll([](std::shared_ptr<ActorBase> const& actor) {
      return actor->isIdle();
    });
  }

  auto finish(ActorPID pid) -> void {
    auto actor = actors.find(pid.id);
    if (actor.has_value()) {
      actor.value()->finish();
    }
  }

  // TODO call this function regularly
  auto garbageCollect() {
    actors.removeIf([](std::shared_ptr<ActorBase> const& actor) -> bool {
      return actor->isFinishedAndIdle();
    });
  }

  auto softShutdown() -> void {
    actors.apply(
        [](std::shared_ptr<ActorBase> const& actor) { actor->finish(); });
    garbageCollect();  // TODO call gc several times with some timeout
  }

  ServerID myServerID;
  std::string const runtimeID;

  std::shared_ptr<IScheduler> scheduler;
  std::shared_ptr<IExternalDispatcher> externalDispatcher;

  // actor id 0 is reserved for special messages
  std::atomic<size_t> uniqueActorIDCounter{1};

  ActorList actors;

 private:
  template<typename ActorMessage>
  auto dispatchLocally(ActorPID sender, ActorPID receiver,
                       ActorMessage const& message) -> void {
    auto actor = actors.find(receiver.id);
    auto payload = MessagePayload<ActorMessage>(std::move(message));
    if (actor.has_value()) {
      actor->get()->process(sender, payload);
    } else {
      dispatch(receiver, sender,
               message::ActorError<ActorPID>{
                   message::ActorNotFound<ActorPID>{.actor = receiver}});
    }
  }

  template<typename ActorMessage>
  auto dispatchExternally(ActorPID sender, ActorPID receiver,
                          ActorMessage const& message) -> void {
    auto payload = inspection::serializeWithErrorT(message);
    ACTOR_ASSERT(payload.ok());
    externalDispatcher->dispatch(sender, receiver, payload.get());
  }
};
template<typename Inspector>
auto inspect(Inspector& f, DistributedRuntime& x) {
  return f.object(x).fields(
      f.field("myServerID", x.myServerID), f.field("runtimeID", x.runtimeID),
      f.field("uniqueActorIDCounter", x.uniqueActorIDCounter.load()),
      f.field("actors", x.actors));
}

};  // namespace arangodb::actor
