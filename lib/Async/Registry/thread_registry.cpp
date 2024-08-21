#include "thread_registry.h"

#include "Assertions/ProdAssert.h"
#include "Async/Registry/Metrics.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"

using namespace arangodb::async_registry;

auto ThreadRegistry::make(std::shared_ptr<const Metrics> metrics)
    -> std::shared_ptr<ThreadRegistry> {
  return std::shared_ptr<ThreadRegistry>(new ThreadRegistry{metrics});
}

ThreadRegistry::ThreadRegistry(std::shared_ptr<const Metrics> metrics)
    : thread_registries_count{*metrics->coroutine_thread_registries, 1},
      running_coroutines{*metrics->running_coroutines},
      coroutines_ready_for_deletion{*metrics->coroutines_ready_for_deletion} {
  if (metrics->coroutine_thread_registries == nullptr) {
    LOG_TOPIC("4be6e", WARN, Logger::STARTUP)
        << "An async thread registry was created with empty metrics.";
  }
}

auto ThreadRegistry::add(PromiseInList* promise) noexcept -> void {
  // promise needs to live on the same thread as this registry
  ADB_PROD_ASSERT(std::this_thread::get_id() == owning_thread);
  auto current_head = promise_head.load(std::memory_order_relaxed);
  promise->next = current_head;
  promise->registry = shared_from_this();
  if (current_head != nullptr) {
    current_head->previous = promise;
  }
  // (1) - this store synchronizes with load in (2)
  promise_head.store(promise, std::memory_order_release);
  running_coroutines.add(1);
}

auto ThreadRegistry::mark_for_deletion(PromiseInList* promise) noexcept
    -> void {
  // makes sure that promise is really in this list
  ADB_PROD_ASSERT(promise->registry.get() == this);
  auto current_head = free_head.load(std::memory_order_relaxed);
  do {
    promise->next_to_free = current_head;
    // (4) - this compare_exchange_weak synchronizes with exchange in (5)
  } while (not free_head.compare_exchange_weak(current_head, promise,
                                               std::memory_order_release,
                                               std::memory_order_acquire));
  // decrement the registries ref-count
  promise->registry.reset();
  running_coroutines.sub(1);
  coroutines_ready_for_deletion.add(1);
}

auto ThreadRegistry::garbage_collect() noexcept -> void {
  ADB_PROD_ASSERT(weak_from_this().expired() ||
                  std::this_thread::get_id() == owning_thread);
  // (5) - this exchange synchronizes with compare_exchange_weak in (4)
  PromiseInList *current,
      *next = free_head.exchange(nullptr, std::memory_order_acquire);
  auto guard = std::lock_guard(mutex);
  while (next != nullptr) {
    current = next;
    next = next->next_to_free;
    remove(current);
    current->destroy();
    coroutines_ready_for_deletion.sub(1);
  }
}

auto ThreadRegistry::remove(PromiseInList* promise) -> void {
  auto next = promise->next;
  auto previous = promise->previous;
  if (previous == nullptr) {  // promise is current head
    // (3) - this store synchronizes with the load in (2)
    promise_head.store(next, std::memory_order_release);
  } else {
    previous->next = next;
  }
  if (next != nullptr) {
    next->previous = previous;
  }
}
