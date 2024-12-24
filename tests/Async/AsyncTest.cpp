#include "Async/async.h"
#include "Async/Registry/promise.h"
#include "Async/Registry/registry_variable.h"
#include "Async/Registry/registry.h"
#include "Async/Registry/thread_registry.h"
#include "Inspection/Format.h"
#include "Inspection/JsonPrintInspector.h"
#include "Utils/ExecContext.h"

#include <gtest/gtest.h>
#include <coroutine>
#include <thread>
#include <deque>
#include <variant>

namespace {

auto promise_count(arangodb::async_registry::ThreadRegistry& registry) -> uint {
  uint promise_count = 0;
  registry.for_promise([&](arangodb::async_registry::PromiseSnapshot promise) {
    promise_count++;
  });
  return promise_count;
}

struct WaitSlot {
  void resume() {
    ready = true;
    _continuation.resume();
  }

  void await() {}

  std::coroutine_handle<> _continuation;

  bool await_ready() { return ready; }
  void await_resume() {}
  void await_suspend(std::coroutine_handle<> continuation) {
    _continuation = continuation;
  }

  void stop() {}

  bool ready = false;
};

struct NoWait {
  void resume() {}
  void await() {}

  auto operator co_await() { return std::suspend_never{}; }
  void stop() {}
};

struct ConcurrentNoWait {
  void resume() {}
  void await() {
    await_suspend(std::noop_coroutine());
    _thread.join();
  }

  bool await_ready() { return false; }
  void await_resume() {}
  void await_suspend(std::coroutine_handle<> handle) {
    {
      std::unique_lock guard(_mutex);
      _coro.emplace_back(handle);
    }
    _cv.notify_one();
  }
  ConcurrentNoWait()
      : _thread([&] {
          bool stopping = false;
          while (true) {
            std::coroutine_handle<> handle;
            {
              std::unique_lock guard(_mutex);
              if (_coro.empty() && stopping) {
                break;
              }
              _cv.wait(guard, [&] { return !_coro.empty(); });
              handle = _coro.front();
              _coro.pop_front();
            }
            if (handle == std::noop_coroutine()) {
              stopping = true;
            }
            handle.resume();
          }
        }) {}

  auto stop() -> void {
    if (_thread.joinable()) {
      await_suspend(std::noop_coroutine());
      _thread.join();
    }
  }

  std::mutex _mutex;
  std::condition_variable_any _cv;
  std::deque<std::coroutine_handle<>> _coro;

  std::jthread _thread;
};

struct InstanceCounterValue {
  InstanceCounterValue() { instanceCounter += 1; }
  InstanceCounterValue(InstanceCounterValue const& o) { instanceCounter += 1; }
  InstanceCounterValue(InstanceCounterValue&& o) noexcept {
    instanceCounter += 1;
  }

  ~InstanceCounterValue() {
    if (instanceCounter == 0) {
      abort();
    }
    instanceCounter -= 1;
  }
  static std::size_t instanceCounter;
};

std::size_t InstanceCounterValue::instanceCounter = 0;

struct CopyOnlyValue : InstanceCounterValue {
  CopyOnlyValue(int x) : x(x) {}
  CopyOnlyValue(CopyOnlyValue const& x) : x(x) {}

  auto operator<=>(int y) const noexcept { return x <=> y; }
  operator int() const noexcept { return x; }
  int x;
};

struct MoveOnlyValue : InstanceCounterValue {
  MoveOnlyValue(int x) : x(x) {}
  MoveOnlyValue(MoveOnlyValue&& x) : x(x) {}
  MoveOnlyValue(MoveOnlyValue const& x) = delete;

  auto operator<=>(int y) const noexcept { return x <=> y; }
  operator int() const noexcept { return x; }
  int x;
};

}  // namespace

template<typename>
struct AsyncTest;

template<typename WaitType, typename ValueType>
struct AsyncTest<std::pair<WaitType, ValueType>> : ::testing::Test {
  void SetUp() override { InstanceCounterValue::instanceCounter = 0; }

  void TearDown() override {
    arangodb::async_registry::get_thread_registry().garbage_collect();
    wait.stop();
    EXPECT_EQ(InstanceCounterValue::instanceCounter, 0);
    EXPECT_EQ(promise_count(arangodb::async_registry::get_thread_registry()),
              0);
    EXPECT_TRUE(std::holds_alternative<arangodb::async_registry::ThreadId>(
        *arangodb::async_registry::get_current_coroutine()));
  }

  WaitType wait;
};

using MyTypes = ::testing::Types<
    std::pair<NoWait, CopyOnlyValue>, std::pair<WaitSlot, MoveOnlyValue>,
    std::pair<ConcurrentNoWait, MoveOnlyValue>,
    std::pair<NoWait, CopyOnlyValue>, std::pair<WaitSlot, MoveOnlyValue>,
    std::pair<ConcurrentNoWait, CopyOnlyValue>>;
TYPED_TEST_SUITE(AsyncTest, MyTypes);

using namespace arangodb;

TYPED_TEST(AsyncTest, async_return) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    co_return 12;
  }();

  this->wait.resume();
  EXPECT_TRUE(a.valid());
  auto awaitable = std::move(a).operator co_await();
  EXPECT_FALSE(a.valid());
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_EQ(awaitable.await_resume(), 12);
}

TYPED_TEST(AsyncTest, async_return_move) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    co_return 12;
  }();

  EXPECT_TRUE(a.valid());

  auto b = std::move(a);
  EXPECT_TRUE(b.valid());
  EXPECT_FALSE(a.valid());

  a = std::move(b);
  EXPECT_TRUE(a.valid());
  EXPECT_FALSE(b.valid());

  this->wait.resume();
  this->wait.await();
}

TYPED_TEST(AsyncTest, async_return_destroy) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    co_return 12;
  }();

  this->wait.resume();
  EXPECT_TRUE(a.valid());
  a.reset();
  EXPECT_FALSE(a.valid());

  this->wait.await();
}

TYPED_TEST(AsyncTest, await_ready_async) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    co_return 12;
  }();

  auto b = [&]() -> async<ValueType> { co_return 2 * co_await std::move(a); }();

  this->wait.resume();
  EXPECT_TRUE(b.valid());
  EXPECT_FALSE(a.valid());
  auto awaitable = std::move(b).operator co_await();
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_EQ(awaitable.await_resume(), 24);
}

TYPED_TEST(AsyncTest, async_throw) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    throw std::runtime_error("TEST!");
  }();

  this->wait.resume();
  EXPECT_TRUE(a.valid());
  auto awaitable = std::move(a).operator co_await();
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_THROW(awaitable.await_resume(), std::runtime_error);
}

TYPED_TEST(AsyncTest, await_throw_async) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    throw std::runtime_error("TEST!");
  }();

  auto b = [&]() -> async<ValueType> {
    try {
      co_return 2 * co_await std::move(a);
    } catch (std::runtime_error const&) {
      co_return 0;
    }
  }();

  this->wait.resume();
  EXPECT_TRUE(b.valid());
  EXPECT_FALSE(a.valid());
  auto awaitable = std::move(b).operator co_await();
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_EQ(awaitable.await_resume(), 0);
}

TYPED_TEST(AsyncTest, await_async_void) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<void> {
    co_await this->wait;
    co_return;
  }();

  auto b = [&]() -> async<ValueType> {
    co_await std::move(a);
    co_return 2;
  }();

  this->wait.resume();
  EXPECT_TRUE(b.valid());
  EXPECT_FALSE(a.valid());
  auto awaitable = std::move(b).operator co_await();
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_EQ(awaitable.await_resume(), 2);
}

TYPED_TEST(AsyncTest, await_async_void_exception) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<void> {
    co_await this->wait;
    throw std::runtime_error("TEST!");
  }();

  auto b = [&]() -> async<ValueType> {
    try {
      co_await std::move(a);
      co_return 2;
    } catch (std::runtime_error const&) {
      co_return 0;
    }
  }();

  this->wait.resume();
  EXPECT_TRUE(b.valid());
  EXPECT_FALSE(a.valid());
  auto awaitable = std::move(b).operator co_await();
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_EQ(awaitable.await_resume(), 0);
}

TYPED_TEST(AsyncTest, multiple_suspension_points) {
  using ValueType = TypeParam::second_type;

  auto a = [&]() -> async<ValueType> {
    co_await this->wait;
    co_return 12;
  };

  auto lambda = [&]() -> async<ValueType> {
    for (int i = 0; i < 10; i++) {
      co_await a();
    }

    co_return 0;
  };

  auto b = lambda();

  this->wait.resume();
  EXPECT_TRUE(b.valid());
  auto awaitable = std::move(b).operator co_await();
  this->wait.await();
  EXPECT_TRUE(awaitable.await_ready());
  EXPECT_EQ(awaitable.await_resume(), 0);
}

TYPED_TEST(AsyncTest, coroutine_is_removed_before_registry_entry) {
  using ValueType = TypeParam::second_type;

  auto coro = []() -> async<ValueType> { co_return 12; };

  {
    coro().reset();
    EXPECT_EQ(InstanceCounterValue::instanceCounter, 0);
    EXPECT_EQ(promise_count(arangodb::async_registry::get_thread_registry()),
              1);
  }
  {
    std::move(coro()).operator co_await().await_resume();
    EXPECT_EQ(InstanceCounterValue::instanceCounter, 0);
    EXPECT_EQ(promise_count(arangodb::async_registry::get_thread_registry()),
              2);
  }
  {
    { std::ignore = coro(); }

    EXPECT_EQ(InstanceCounterValue::instanceCounter, 0);
    EXPECT_EQ(promise_count(arangodb::async_registry::get_thread_registry()),
              3);
  }
}

namespace {
auto foo() -> async<void> { co_return; }
auto bar() -> async<void> { co_return; }
auto baz() -> async<void> { co_return; }
}  // namespace
TYPED_TEST(AsyncTest, promises_are_registered_in_global_async_registry) {
  auto coro_foo = foo();
  EXPECT_EQ(promise_count(arangodb::async_registry::get_thread_registry()), 1);

  std::jthread([&]() {
    auto coro_bar = bar();
    auto coro_baz = baz();

    std::vector<std::string_view> names;
    arangodb::async_registry::registry.for_promise(
        [&](arangodb::async_registry::PromiseSnapshot promise) {
          names.push_back(promise.source_location.function_name);
        });
    EXPECT_EQ(names.size(), 3);
    EXPECT_TRUE(names[0].find("foo") != std::string::npos);
    EXPECT_TRUE(names[1].find("baz") != std::string::npos);
    EXPECT_TRUE(names[2].find("bar") != std::string::npos);
  }).join();
}

struct ExecContext_Waiting : public arangodb::ExecContext {
  ExecContext_Waiting()
      : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                              arangodb::ExecContext::Type::Default, "Waiting",
                              "", arangodb::auth::Level::RW,
                              arangodb::auth::Level::NONE, true) {}
};
struct ExecContext_Calling : public arangodb::ExecContext {
  ExecContext_Calling()
      : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                              arangodb::ExecContext::Type::Default, "Calling",
                              "", arangodb::auth::Level::RW,
                              arangodb::auth::Level::NONE, true) {}
};
struct ExecContext_Begin : public arangodb::ExecContext {
  ExecContext_Begin()
      : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                              arangodb::ExecContext::Type::Default, "Begin", "",
                              arangodb::auth::Level::RW,
                              arangodb::auth::Level::NONE, true) {}
};
struct ExecContext_End : public arangodb::ExecContext {
  ExecContext_End()
      : arangodb::ExecContext(arangodb::ExecContext::ConstructorToken{},
                              arangodb::ExecContext::Type::Default, "End", "",
                              arangodb::auth::Level::RW,
                              arangodb::auth::Level::NONE, true) {}
};
TYPED_TEST(AsyncTest, execution_context_is_local_to_coroutine) {
  ExecContextScope exec(std::make_shared<ExecContext_Begin>());
  EXPECT_EQ(ExecContext::current().user(), "Begin");

  auto waiting_coro = [&]() -> async<void> {
    EXPECT_EQ(ExecContext::current().user(), "Begin");
    ExecContextScope exec(std::make_shared<ExecContext_Waiting>());
    EXPECT_EQ(ExecContext::current().user(), "Waiting");
    co_await this->wait;
    EXPECT_EQ(ExecContext::current().user(), "Waiting");
    co_return;
  }();
  EXPECT_EQ(ExecContext::current().user(), "Begin");

  auto trivial_coro = []() -> async<void> {
    EXPECT_EQ(ExecContext::current().user(), "Begin");
    co_return;
  }();

  auto calling_coro = [&]() -> async<void> {
    EXPECT_EQ(ExecContext::current().user(), "Begin");
    ExecContextScope exec(std::make_shared<ExecContext_Calling>());
    EXPECT_EQ(ExecContext::current().user(), "Calling");
    co_await std::move(waiting_coro);
    EXPECT_EQ(ExecContext::current().user(), "Calling");
    co_await std::move(trivial_coro);
    EXPECT_EQ(ExecContext::current().user(), "Calling");
    co_return;
  };
  EXPECT_EQ(ExecContext::current().user(), "Begin");

  std::ignore = calling_coro();
  EXPECT_EQ(ExecContext::current().user(), "Begin");

  ExecContextScope new_exec(std::make_shared<ExecContext_End>());
  EXPECT_EQ(ExecContext::current().user(), "End");

  this->wait.resume();
  this->wait.await();
  EXPECT_EQ(ExecContext::current().user(), "End");
}

namespace {
auto find_promise_by_name(std::string_view name)
    -> std::optional<async_registry::PromiseSnapshot> {
  std::optional<async_registry::PromiseSnapshot> requested_promise =
      std::nullopt;
  arangodb::async_registry::registry.for_promise(
      [&](arangodb::async_registry::PromiseSnapshot promise) {
        if (promise.source_location.function_name.find(name) !=
            std::string::npos) {
          requested_promise = promise;
        }
      });
  return requested_promise;
}
}  // namespace
TYPED_TEST(
    AsyncTest,
    async_promises_in_async_registry_know_their_requester_with_nested_coroutines) {
  using TestType = decltype(this);
  arangodb::async_registry::get_thread_registry().garbage_collect();
  struct Functions {
    static auto awaited_by_awaited_fn(TestType test) -> async<void> {
      auto promise = find_promise_by_name("awaited_by_awaited_fn");
      EXPECT_TRUE(promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::PromiseId>(
          promise->requester));
      co_await test->wait;

      co_return;
    };
    static auto awaited_fn(TestType test) -> async<void> {
      auto promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::PromiseId>(
          promise->requester));

      auto fn = Functions::awaited_by_awaited_fn(test);
      auto awaited_promise = find_promise_by_name("awaited_by_awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{promise->id});

      co_await std::move(fn);
      awaited_promise = find_promise_by_name("awaited_by_awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{promise->id});

      co_return;
    };
    static auto waiter_fn(TestType test) -> async<void> {
      auto waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      auto fn = Functions::awaited_fn(test);

      auto awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{waiter_promise->id});

      co_await std::move(fn);

      awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{waiter_promise->id});

      // waiter did not change
      waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      co_return;
    };
  };

  std::ignore = Functions::waiter_fn(this);

  this->wait.resume();
  this->wait.await();
}

TYPED_TEST(
    AsyncTest,
    async_promises_in_async_registry_know_their_requester_with_two_requests_after_each_other) {
  using TestType = decltype(this);
  arangodb::async_registry::get_thread_registry().garbage_collect();
  struct Functions {
    static auto awaited_2_fn() -> async<void> {
      auto promise = find_promise_by_name("awaited_2_fn");
      EXPECT_TRUE(promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::PromiseId>(
          promise->requester));

      co_return;
    };
    static auto awaited_fn(TestType test) -> async<void> {
      auto promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::PromiseId>(
          promise->requester));

      co_await test->wait;

      co_return;
    };
    static auto waiter_fn(TestType test) -> async<void> {
      auto waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      auto fn = Functions::awaited_fn(test);
      auto fn_2 = Functions::awaited_2_fn();

      auto awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{waiter_promise->id});
      auto awaited_2_promise = find_promise_by_name("awaited_2_fn");
      EXPECT_TRUE(awaited_2_promise.has_value());
      EXPECT_EQ(awaited_2_promise->requester,
                async_registry::Requester{waiter_promise->id});

      co_await std::move(fn);
      awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{waiter_promise->id});

      co_await std::move(fn_2);
      awaited_2_promise = find_promise_by_name("awaited_2_fn");
      EXPECT_TRUE(awaited_2_promise.has_value());
      EXPECT_EQ(awaited_2_promise->requester,
                async_registry::Requester{waiter_promise->id});

      // waiter did not change
      waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      co_return;
    };
  };

  std::ignore = Functions::waiter_fn(this);

  this->wait.resume();
  this->wait.await();
}

TYPED_TEST(AsyncTest,
           async_promises_in_async_registry_know_their_requester_with_move) {
  arangodb::async_registry::get_thread_registry().garbage_collect();
  using TestType = decltype(this);
  struct Functions {
    static auto awaited_fn(TestType test) -> async<void> {
      auto promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(promise.has_value());
      EXPECT_TRUE(
          std::holds_alternative<async_registry::ThreadId>(promise->requester));

      co_await test->wait;

      co_return;
    };
    static auto waiter_fn(async<void>&& fn) -> async<void> {
      auto waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      auto awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      co_await std::move(fn);

      awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{waiter_promise->id});

      // waiter did not change
      waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      co_return;
    };
  };

  auto awaited_coro = Functions::awaited_fn(this);
  std::ignore = Functions::waiter_fn(std::move(awaited_coro));

  this->wait.resume();
  this->wait.await();
}

TYPED_TEST(
    AsyncTest,
    async_promises_in_async_registry_know_their_requester_with_move_and_call_without_await) {
  arangodb::async_registry::get_thread_registry().garbage_collect();
  using TestType = decltype(this);
  struct Functions {
    static auto awaited_2_fn(TestType test) -> async<void> {
      co_await test->wait;
      co_return;
    };
    static auto awaited_fn() -> async<void> { co_return; };
    static auto waiter_fn(async<void>&& fn, TestType test) -> async<void> {
      co_await std::move(fn);

      auto a = awaited_2_fn(test);

      auto waiter_promise = find_promise_by_name("waiter_fn");
      EXPECT_TRUE(waiter_promise.has_value());
      EXPECT_TRUE(std::holds_alternative<async_registry::ThreadId>(
          waiter_promise->requester));

      auto awaited_promise = find_promise_by_name("awaited_fn");
      EXPECT_TRUE(awaited_promise.has_value());
      EXPECT_EQ(awaited_promise->requester,
                async_registry::Requester{waiter_promise->id});

      auto awaited_2_promise = find_promise_by_name("awaited_2_fn");
      EXPECT_TRUE(awaited_2_promise.has_value());
      EXPECT_EQ(awaited_2_promise->requester,
                async_registry::Requester{waiter_promise->id});

      co_return;
    };
  };

  auto awaited_coro = Functions::awaited_fn();
  std::ignore = Functions::waiter_fn(std::move(awaited_coro), this);

  this->wait.resume();
  this->wait.await();
}

namespace {
auto expect_all_promises_in_state(arangodb::async_registry::State state,
                                  uint number_of_promises) {
  uint count = 0;
  arangodb::async_registry::registry.for_promise(
      [&](arangodb::async_registry::PromiseSnapshot promise) {
        count++;
        EXPECT_EQ(promise.state, state);
      });
  EXPECT_EQ(count, number_of_promises);
}
}  // namespace
TYPED_TEST(AsyncTest, async_promises_in_async_registry_know_their_state) {
  {
    auto coro = [&]() -> async<int> {
      co_await this->wait;
      co_return 12;
    }();

    if (std::is_same<decltype(this->wait), WaitSlot>()) {
      expect_all_promises_in_state(arangodb::async_registry::State::Suspended,
                                   1);
    }

    this->wait.resume();
    this->wait.await();

    expect_all_promises_in_state(arangodb::async_registry::State::Resolved, 1);
  }

  expect_all_promises_in_state(arangodb::async_registry::State::Deleted, 1);
}

#include "AsyncTestLineNumbers.tpp"
