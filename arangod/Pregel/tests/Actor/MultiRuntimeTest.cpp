////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
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

#include <gtest/gtest.h>
#include <unordered_set>

#include "Pregel/Actor/ActorPID.h"
#include "Pregel/Actor/Runtime.h"

#include "Pregel/Actor/Actors/TrivialActor.h"
#include "Pregel/Actor/Actors/PingPongActors.h"

using namespace arangodb;

using namespace arangodb::pregel::actor;
using namespace arangodb::pregel::actor::test;

struct MockScheduler {
  auto operator()(auto fn) { fn(); }
};

template<typename Runtime>
struct MockExternalDispatcher {
  MockExternalDispatcher(
      std::unordered_map<ServerID, std::shared_ptr<Runtime>>& runtimes)
      : runtimes(runtimes) {}
  auto operator()(ActorPID sender, ActorPID receiver,
                  arangodb::velocypack::SharedSlice msg) -> void {
    auto receiving_runtime = runtimes.find(receiver.server);
    if (receiving_runtime != std::end(runtimes)) {
      receiving_runtime->second->dispatch(sender, receiver, msg);
    } else {
      auto error = ActorError{ActorNotFound{.actor = receiver}};
      auto payload = inspection::serializeWithErrorT(error);
      if (payload.ok()) {
        runtimes[sender.server]->dispatch(receiver, sender, payload.get());
      } else {
        fmt::print("Error serializing ActorNotFound");
        std::abort();
      }
    }
  }
  std::unordered_map<ServerID, std::shared_ptr<Runtime>>& runtimes;
};

struct MockRuntime
    : Runtime<MockScheduler, MockExternalDispatcher<MockRuntime>> {
  using Runtime<MockScheduler, MockExternalDispatcher<MockRuntime>>::Runtime;
};

TEST(MultiRuntimeTest, ping_pong_game) {
  std::unordered_map<ServerID, std::shared_ptr<MockRuntime>> runtimes;

  auto scheduler = std::make_shared<MockScheduler>();
  auto dispatcher =
      std::make_shared<MockExternalDispatcher<MockRuntime>>(runtimes);

  // Runtime A with pong actor
  auto serverIDA = ServerID{"A"};
  runtimes.emplace(serverIDA,
                   std::make_shared<MockRuntime>(serverIDA, "RuntimeTest-A",
                                                 scheduler, dispatcher));
  auto pong_actor = runtimes[serverIDA]->spawn<pong_actor::Actor>(
      pong_actor::State{}, pong_actor::Start{});

  // Runtime B with ping actor: sends pong message
  auto serverIDB = ServerID{"B"};
  runtimes.emplace(serverIDB,
                   std::make_shared<MockRuntime>(serverIDB, "RuntimeTest-B",
                                                 scheduler, dispatcher));
  auto ping_actor = runtimes[serverIDB]->spawn<ping_actor::Actor>(
      ping_actor::State{},
      ping_actor::Start{.pongActor =
                            ActorPID{.server = serverIDA, .id = pong_actor}});

  auto ping_actor_state =
      runtimes[serverIDB]->getActorStateByID<ping_actor::Actor>(ping_actor);
  ASSERT_EQ(ping_actor_state,
            (ping_actor::State{.called = 2, .message = "hello world"}));
  auto pong_actor_state =
      runtimes[serverIDA]->getActorStateByID<pong_actor::Actor>(pong_actor);
  ASSERT_EQ(pong_actor_state, (pong_actor::State{.called = 2}));
}

TEST(MultiRuntimeTest,
     actor_receiving_wrong_message_type_sends_back_unknown_error_message) {
  struct SomeMessage {};
  struct SomeMessages : std::variant<SomeMessage> {
    using std::variant<SomeMessage>::variant;
  };

  std::unordered_map<ServerID, std::shared_ptr<MockRuntime>> runtimes;

  auto scheduler = std::make_shared<MockScheduler>();
  auto dispatcher =
      std::make_shared<MockExternalDispatcher<MockRuntime>>(runtimes);

  // Runtime A with trivial actor
  auto serverIDA = ServerID{"A"};
  runtimes.emplace(serverIDA,
                   std::make_shared<MockRuntime>(serverIDA, "RuntimeTest-A",
                                                 scheduler, dispatcher));
  auto trivial_actor = runtimes[serverIDA]->spawn<TrivialActor>(
      TrivialState{.state = "foo"}, TrivialMessage0{});

  // Runtime B with ping actor: sends pong message to trivial actor
  auto serverIDB = ServerID{"B"};
  runtimes.emplace(serverIDB,
                   std::make_shared<MockRuntime>(serverIDB, "RuntimeTest-B",
                                                 scheduler, dispatcher));
  auto ping_actor = runtimes[serverIDB]->spawn<ping_actor::Actor>(
      ping_actor::State{},
      ping_actor::Start{
          .pongActor = ActorPID{.server = serverIDA, .id = trivial_actor}});

  // trivial actor was called once (with an for it unknown message type pong
  // message)
  ASSERT_EQ(runtimes[serverIDA]->getActorStateByID<TrivialActor>(trivial_actor),
            (TrivialState{.state = "foo", .called = 1}));
  // ping actor received an unknown message error after it sent wrong message
  // type to trivial actor
  ASSERT_EQ(
      runtimes[serverIDB]->getActorStateByID<ping_actor::Actor>(ping_actor),
      (ping_actor::State{
          .called = 2,
          .message = "sent unknown message",
      }));
}

TEST(
    MultiRuntimeTest,
    actor_receives_actor_not_found_message_after_trying_to_send_message_to_non_existent_actor_on_another_server) {
  std::unordered_map<ServerID, std::shared_ptr<MockRuntime>> runtimes;

  auto scheduler = std::make_shared<MockScheduler>();
  auto dispatcher =
      std::make_shared<MockExternalDispatcher<MockRuntime>>(runtimes);

  // Runtime A with no actors
  auto serverIDA = ServerID{"A"};
  runtimes.emplace(serverIDA,
                   std::make_shared<MockRuntime>(serverIDA, "RuntimeTest-A",
                                                 scheduler, dispatcher));

  // Runtime B with ping actor: sends pong message to unknown actor in runtime A
  auto serverIDB = ServerID{"B"};
  runtimes.emplace(serverIDB,
                   std::make_shared<MockRuntime>(serverIDB, "RuntimeTest-B",
                                                 scheduler, dispatcher));
  auto unknownActorPID = ActorPID{.server = "A", .id = {999}};
  auto ping_actor = runtimes[serverIDB]->spawn<ping_actor::Actor>(
      ping_actor::State{}, ping_actor::Start{.pongActor = unknownActorPID});

  // ping actor received an actor not known message error after it message
  // to non-existing actor in other runtime
  ASSERT_EQ(
      runtimes[serverIDB]->getActorStateByID<ping_actor::Actor>(ping_actor),
      (ping_actor::State{.called = 2,
                         .message = fmt::format("recieving actor {} not found",
                                                unknownActorPID)}));
}

// TODO extra type of error: ServerNotFound?
TEST(
    MultiRuntimeTest,
    actor_receives_actor_not_found_message_after_trying_to_send_message_to_non_existent_server) {
  std::unordered_map<ServerID, std::shared_ptr<MockRuntime>> runtimes;

  auto scheduler = std::make_shared<MockScheduler>();
  auto dispatcher =
      std::make_shared<MockExternalDispatcher<MockRuntime>>(runtimes);

  // Runtime i with ping actor: sends pong message to unknown actor on another
  // server
  auto unknownActorPID = ActorPID{.server = "B", .id = {999}};
  auto serverIDA = ServerID{"A"};
  runtimes.emplace(serverIDA,
                   std::make_shared<MockRuntime>(serverIDA, "RuntimeTest-B",
                                                 scheduler, dispatcher));
  auto ping_actor = runtimes[serverIDA]->spawn<ping_actor::Actor>(
      ping_actor::State{}, ping_actor::Start{.pongActor = unknownActorPID});

  // ping actor received an actor not known message error after it message
  // to non-existing runtime
  ASSERT_EQ(
      runtimes[serverIDA]->getActorStateByID<ping_actor::Actor>(ping_actor),
      (ping_actor::State{.called = 2,
                         .message = fmt::format("recieving actor {} not found",
                                                unknownActorPID)}));
}
