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

#include <chrono>
#include <thread>
#include <string>
#include <memory>
#include <variant>

#include "Actor/Runtime.h"
#include "Actor/Actor.h"

namespace arangodb::pregel::actor::test {

namespace pong_actor {
struct Ping;
}

namespace ping_actor {

struct State {
  bool operator==(const State&) const = default;
};

struct Start {
  ActorPID pongActor;
};

struct Pong {
  std::string text;
};

struct Handler : HandlerBase<State> {
  auto operator()(Start msg) -> std::unique_ptr<State> {
    std::cout << "pong actor: " << msg.pongActor.server << " "
              << msg.pongActor.id.id << std::endl;
    // runtime.send(msg.pongActor, pong_actor::Ping{.text = "hello world"});
    return std::move(state);
  }

  auto operator()(Pong msg) -> std::unique_ptr<State> {
    return std::move(state);
  }
};

struct Actor {
  using State = State;
  using Handler = Handler;
  using Message = std::variant<Start, Pong>;
  static constexpr auto typeName() -> std::string_view { return "PongActor"; };
};

}  // namespace ping_actor

namespace pong_actor {

struct State {
  bool operator==(const State&) const = default;
};

struct Start {};

struct Ping {
  ActorPID sender;
  std::string text;
};

struct Handler : HandlerBase<State> {
  auto operator()(Start msg) -> std::unique_ptr<State> {
    return std::move(state);
  }

  auto operator()(Ping msg) -> std::unique_ptr<State> {
    // runtime.send(msg.sender, ping_actor::Pong{.text = msg.text});
    return std::move(state);
  }
};

struct Actor {
  using State = State;
  using Handler = Handler;
  using Message = std::variant<Start, Ping>;
  static constexpr auto typeName() -> std::string_view { return "PingActor"; };
};

}  // namespace pong_actor

}  // namespace arangodb::pregel::actor::test
