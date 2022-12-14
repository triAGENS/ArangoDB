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
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <Inspection/VPackWithErrorT.h>
#include <memory>
#include <iostream>
#include <string_view>
#include <type_traits>

#include "ActorPID.h"
#include "Message.h"
#include "MPSCQueue.h"

namespace arangodb::pregel::actor {

struct Dispatcher;

struct ActorBase {
  virtual ~ActorBase() = default;
  virtual auto process(ActorPID sender,
                       std::unique_ptr<MessagePayloadBase> payload) -> void = 0;
  virtual auto typeName() -> std::string_view = 0;
  // state: initialised, running, finished
};

template<typename State>
struct HandlerBase {
  HandlerBase(ActorPID pid, std::shared_ptr<Dispatcher> messageDispatcher,
              std::unique_ptr<State> state)
      : pid(pid),
        messageDispatcher(messageDispatcher),
        state{std::move(state)} {};
  ActorPID pid;
  std::shared_ptr<Dispatcher> messageDispatcher;
  std::unique_ptr<State> state;
};

namespace {
template<typename A>
concept IncludesAllActorRelevantTypes =
    std::is_class<typename A::State>::value &&
    std::is_class<typename A::Handler>::value &&
    std::is_class<typename A::Message>::value;
template<typename A>
concept IncludesName = requires() {
  { A::typeName() } -> std::convertible_to<std::string_view>;
};
template<typename A>
concept HandlerInheritsFromBaseHandler =
    std::is_base_of < HandlerBase<typename A::State>,
typename A::Handler > ::value;
template<typename A>
concept MessageIsVariant = requires(ActorPID pid, std::shared_ptr<Dispatcher> messageDispatcher,
                                    typename A::State state,
                                    typename A::Message message) {
{std::visit(typename A::Handler{{pid, messageDispatcher,
                                  std::move(std::make_unique<typename A::State>(
                                              state))}},
              message)};
};
};  // namespace
template<typename A>
concept Actorable = IncludesAllActorRelevantTypes<A> && IncludesName<A> &&
    HandlerInheritsFromBaseHandler<A> && MessageIsVariant<A>;

template<typename Scheduler, Actorable Config>
struct Actor : ActorBase {
  Actor(ActorPID pid, std::shared_ptr<Scheduler> schedule,
        std::shared_ptr<Dispatcher> dispatcher,
        std::unique_ptr<typename Config::State> initialState)
      : schedule(schedule),
        messageDispatcher(dispatcher),
        state(std::move(initialState)),
        pid(pid) {}

  Actor(ActorPID pid, std::shared_ptr<Scheduler> schedule,
        std::shared_ptr<Dispatcher> dispatcher,
        std::unique_ptr<typename Config::State> initialState,
        std::size_t batchSize)
      : batchSize(batchSize),
        schedule(schedule),
        messageDispatcher(dispatcher),
        state(std::move(initialState)),
        pid(pid) {}

  ~Actor() = default;

  auto typeName() -> std::string_view override { return Config::typeName(); };

  void process(ActorPID sender,
               std::unique_ptr<MessagePayloadBase> msg) override {
    auto* ptr = msg.release();
    auto* m = dynamic_cast<MessagePayload<typename Config::Message>*>(ptr);
    if (m == nullptr) {
      // TODO possibly send an information back to the runtime
      std::cout << "actor process found a nullptr payload" << std::endl;
      std::abort();
    }
    inbox.push(std::make_unique<InternalMessage>(
        sender,
        std::make_unique<typename Config::Message>(std::move(m->payload))));
    delete m;
    kick();
  }

  void kick() {
    // Make sure that *someone* works here
    (*schedule)([this]() { this->work(); });
  }

  void work() {
    auto was_false = false;
    if (busy.compare_exchange_strong(was_false, true)) {
      auto i = batchSize;

      while (auto msg = inbox.pop()) {
        if (msg == nullptr) {
          std::cout << "work msg was nullptr" << std::endl;
        }
        if (msg->payload == nullptr) {
          std::cout << "work payload was nullptr" << std::endl;
        }

        state = std::visit(
          typename Config::Handler{ { pid, messageDispatcher, std::move(state)} },
            *msg->payload);
        i--;
        if (i == 0) {
          break;
        }
      }
      busy.store(false);

      if (!inbox.empty()) {
        kick();
      }
    }
  }

  struct InternalMessage
      : arangodb::pregel::mpscqueue::MPSCQueue<InternalMessage>::Node {
    InternalMessage(ActorPID sender,
                    std::unique_ptr<typename Config::Message>&& payload)
        : sender(sender), payload(std::move(payload)) {}
    ActorPID sender;
    std::unique_ptr<typename Config::Message> payload;
  };

  std::size_t batchSize{16};
  std::atomic<bool> busy;
  arangodb::pregel::mpscqueue::MPSCQueue<InternalMessage> inbox;
  std::shared_ptr<Scheduler> schedule;
  std::shared_ptr<Dispatcher> messageDispatcher;
  std::unique_ptr<typename Config::State> state;
  ActorPID pid;
};

}  // namespace arangodb::pregel::actor
