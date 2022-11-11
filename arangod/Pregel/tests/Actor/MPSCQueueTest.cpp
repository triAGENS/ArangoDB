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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include <chrono>
#include <thread>
#include <string>

#include "gtest/gtest.h"
#include "Actor/MPSCQueue.h"
#include "Basics/ThreadGuard.h"

#include "fmt/core.h"

using namespace arangodb;
using namespace arangodb::pregel::mpscqueue;

struct Message : MPSCQueue::Node {
  Message(std::string content) : content(std::move(content)) {}
  std::string content;
};

TEST(MPSCQueue, gives_back_stuff_pushed) {
  auto queue = MPSCQueue();

  queue.push(std::make_unique<Message>("aon"));
  queue.push(std::make_unique<Message>("dha"));
  queue.push(std::make_unique<Message>("tri"));
  queue.push(std::make_unique<Message>("ceithir"));
  queue.push(std::make_unique<Message>("dannsa"));

  auto msg1 = static_cast<Message*>(queue.pop().release());
  ASSERT_EQ("aon", msg1->content);

  auto msg2 = static_cast<Message*>(queue.pop().release());
  ASSERT_EQ("dha", msg2->content);

  auto msg3 = static_cast<Message*>(queue.pop().release());
  ASSERT_EQ("tri", msg3->content);

  auto msg4 = static_cast<Message*>(queue.pop().release());
  ASSERT_EQ("ceithir", msg4->content);

  auto msg5 = static_cast<Message*>(queue.pop().release());
  ASSERT_EQ("dannsa", msg5->content);

  auto msg6 = static_cast<Message*>(queue.pop().release());
  ASSERT_EQ(nullptr, msg6);
}

struct Message2 : MPSCQueue::Node {
  Message2(size_t threadId, size_t messageId)
      : threadId(threadId), messageId(messageId) {}
  size_t threadId{};
  size_t messageId{};
};

// This test starts a number of system threads that push
// messages onto the message queue, and an additional
// thread that keeps reading messages from the queue;
//
// Apart from checking that this process doesn't crash
// the test checks that every message id from every thread
// has been read in the consumer.
TEST(MPSCQueue, threads_push_stuff_comes_out) {
  constexpr auto numberThreads = size_t{125};
  constexpr auto numberMessages = size_t{10000};

  auto queue = MPSCQueue();
  auto threads = ThreadGuard();

  for (auto threadId = size_t{0}; threadId < numberThreads; ++threadId) {
    threads.emplace([&queue, threadId]() {
      for (size_t msgN = 0; msgN < numberMessages; ++msgN) {
        queue.push(std::make_unique<Message2>(threadId, msgN));
      }
    });
  }

  auto receivedIds = std::vector<std::vector<bool>>(numberThreads);
  for (auto& thread : receivedIds) {
    thread = std::vector<bool>(numberMessages);
  }

  threads.emplace([&queue, &receivedIds, numberThreads, numberMessages]() {
    auto counter = size_t{0};

    while (true) {
      auto rcv = queue.pop();
      if (rcv != nullptr) {
        auto msg = static_cast<Message2*>(rcv.release());
        receivedIds[msg->threadId][msg->messageId] = true;
        counter++;

        ASSERT_LT(msg->threadId, numberThreads);
        ASSERT_LE(counter, numberThreads * numberMessages);

        if (counter == numberThreads * numberMessages) {
          break;
        }
      }
    }
  });
  threads.joinAll();

  for (auto& thread : receivedIds) {
    ASSERT_TRUE(std::all_of(std::begin(thread), std::end(thread),
                            [](auto x) { return x; }));
  }
}
