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
/// @author Markus Pfeiffer
/// @author Julia Volmer
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Inspection/VPackWithErrorT.h"

#include "Dispatcher.h"
#include "Message.h"
#include "ActorPID.h"

namespace arangodb::pregel::actor {

template<typename State>
struct HandlerBase {
  HandlerBase(ActorPID self, ActorPID sender, std::unique_ptr<State> state,
              std::shared_ptr<Dispatcher> messageDispatcher)
      : self(self),
        sender(sender),
        state{std::move(state)},
        messageDispatcher(messageDispatcher){};

  template<typename ActorMessage>
  auto dispatch(ActorPID receiver, ActorMessage message) -> void {
    if (receiver.server == self.server) {
      (*messageDispatcher)(
          self, receiver,
          std::make_unique<MessagePayload<ActorMessage>>(std::move(message)));
    } else {
      auto payload = inspection::serializeWithErrorT(message);
      if (payload.ok()) {
        (*messageDispatcher)(self, receiver, payload.get());
      } else {
        fmt::print("HandlerBase error serializing message");
        std::abort();
      }
    }
  }

  ActorPID self;
  ActorPID sender;
  std::unique_ptr<State> state;

 private:
  std::shared_ptr<Dispatcher> messageDispatcher;
};

}  // namespace arangodb::pregel::actor
