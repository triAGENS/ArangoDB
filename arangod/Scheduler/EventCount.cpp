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
////////////////////////////////////////////////////////////////////////////////

#include "EventCount.h"

#include <atomic>
#include <functional>
#include <limits>
#include <vector>

#include "Assertions/Assert.h"

namespace arangodb {

// This EventCount implementation is heavily inspired by the Eigen EventCount
// implementation by Dmitry Vyukov
// https://gitlab.com/libeigen/eigen/-/blob/5e4f3475b5cb77414bca1f3dde7d6fd9cb4d2011/Eigen/src/ThreadPool/EventCount.h

EventCount::PendingWait::PendingWait(EventCount& ec,
                                     std::size_t waiterIndex) noexcept
    : _ec(ec), _waiterIndex(waiterIndex) {}

EventCount::PendingWait::~PendingWait() {
  TRI_ASSERT(_waiterIndex == kInvalidIndex);
  if (_waiterIndex != kInvalidIndex) {
    cancel();
  }
}

void EventCount::PendingWait::commit() noexcept {
  _ec.commitWait(_waiterIndex);
  TRI_ASSERT(_ec._waiters[_waiterIndex].status.load(
                 std::memory_order_relaxed) == Waiter::kActive);
  _waiterIndex = kInvalidIndex;
}

void EventCount::PendingWait::cancel() noexcept {
  TRI_ASSERT(_waiterIndex != kInvalidIndex);
  _ec.cancelWait(_waiterIndex);
  TRI_ASSERT(_ec._waiters[_waiterIndex].status.load(
                 std::memory_order_relaxed) == Waiter::kActive);
  _waiterIndex = kInvalidIndex;
}

EventCount::EventCount(std::size_t numWaiters) noexcept
    : _state(), _waiters(numWaiters) {
  TRI_ASSERT(_waiters.size() < kMaxWaiters);
}

EventCount::~EventCount() { TRI_ASSERT(_state.load() == State{kEmptyStack}); }

#ifdef ARANGODB_USE_GOOGLE_TESTS
auto EventCount::getNumWaiters() const noexcept -> std::size_t {
  return _state.load().waiters();
}

auto EventCount::getNumSignals() const noexcept -> std::size_t {
  return _state.load().signals();
}

auto EventCount::getWaiterStack() const noexcept -> std::vector<std::size_t> {
  std::vector<std::size_t> result;
  auto stackIdx = _state.load().stack();
  while (stackIdx != kInvalidIndex) {
    result.push_back(stackIdx);
    stackIdx =
        State{_waiters[stackIdx].next.load(std::memory_order_relaxed)}.stack();
  }
  return result;
}
#endif

auto EventCount::prepareWait(std::size_t waiterIndex) noexcept -> PendingWait {
  TRI_ASSERT(waiterIndex < _waiters.size());
  auto& w = _waiters[waiterIndex];
  TRI_ASSERT(w.status.load(std::memory_order_relaxed) == Waiter::kActive)
      << "waiterIndex: " << waiterIndex
      << " status: " << w.status.load(std::memory_order_relaxed);
  w.status.store(Waiter::kPreparedWait, std::memory_order_relaxed);
  _state.incWaiter(std::memory_order_seq_cst);
  return PendingWait{*this, waiterIndex};
}

void EventCount::commitWait(std::size_t waiterIndex) noexcept {
  TRI_ASSERT(waiterIndex < _waiters.size());
  Waiter& w = _waiters[waiterIndex];
  TRI_ASSERT((w.epoch & ~kEpochMask) == 0);
  TRI_ASSERT(w.status.load(std::memory_order_relaxed) == Waiter::kPreparedWait);
  w.status.store(Waiter::kNotSignaled, std::memory_order_relaxed);
  std::uint64_t myStackIdx = w.epoch | (waiterIndex << kStackShift);

  // TODO - can this be relaxed?
  auto state = _state.load(std::memory_order_relaxed);
  for (;;) {
    state.check(true);
    auto newState = std::invoke([&]() {
      if (state.signals() != 0) {
        // someone has set a signal -> consume it and return
        return state.decSignal().decWaiter();
      } else {
        // remove ourself from prepareWait counter and add to the waiter stack
        w.next.store(state.value & kEpochStackMask, std::memory_order_relaxed);
        return State{(state.decWaiter().value & kWaiterMask) | myStackIdx};
      }
    });

    if (_state.compare_exchange_weak(state, newState, std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
      if (state.signals() == 0) {
        w.block();
      }
      w.status.store(Waiter::kActive, std::memory_order_relaxed);
      return;
    }
  }
}

void EventCount::cancelWait(std::size_t waiterIndex) noexcept {
  auto state = _state.load(std::memory_order_relaxed);
  auto& w = _waiters[waiterIndex];
  TRI_ASSERT(w.status.load(std::memory_order_relaxed) == Waiter::kPreparedWait);
  for (;;) {
    state.check(true);
    // If signals < waiters we can just decrement the waiter count and return -
    // the remaining signals (if any) will be consumed by the other waiters.
    // However, if waiters == signals this means that we have also received a
    // signal which we must consume in order to maintain the invariant that
    // signals <= waiters.
    auto newState = std::invoke([&]() {
      if (state.waiters() == state.signals()) {
        return state.decSignal().decWaiter();
      } else {
        return state.decWaiter();
      }
    });
    if (_state.compare_exchange_weak(state, newState, std::memory_order_release,
                                     std::memory_order_relaxed)) {
      w.status.store(Waiter::kActive, std::memory_order_relaxed);
      return;
    }
  }
}

auto EventCount::notify(bool notifyAll, std::size_t& notified) noexcept
    -> bool {
  // TODO - do we need this fence
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // TODO
  auto state = _state.load(std::memory_order_acquire);
  for (;;) {
    auto numWaiters = state.waiters();
    auto numSignals = state.signals();
    auto stackIdx = state.stack();
    if (stackIdx == kInvalidIndex && numWaiters == numSignals) {
      // no one to wake!
      return false;
    }

    auto newState = std::invoke([&]() {
      if (notifyAll) {
        // clear wait stack and set signal to number of pre-wait threads.
        numSignals = numWaiters;
        return State{kEmptyStack | (numWaiters << kWaiterShift) | numSignals};
      } else if (numSignals < numWaiters) {
        // there is a thread in prepareWait state -> unblock it.
        return state.incSignal();
      } else {
        // pop a waiter from list and unblock it.
        auto* w = &_waiters[stackIdx];
        auto next = w->next.load(std::memory_order_relaxed);
        TRI_ASSERT((next & ~kEpochStackMask) == 0);
        return State{next | (state.value & (kWaiterMask | kSignalMask))};
      }
    });

    if (_state.compare_exchange_weak(state, newState, std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
      if (!notifyAll && (numSignals < numWaiters)) {
        // unblocked thread in prepareWait state by setting signal
        // -> nothing else to do!
      } else if (stackIdx != kInvalidIndex) {
        if (notifyAll) {
          notified = std::numeric_limits<std::size_t>::max();
          unblockAll(stackIdx);
        } else {
          notified = stackIdx;
          _waiters[stackIdx].unblock();
        }
      }
      return true;
    }
  }
}

void EventCount::unblockAll(std::size_t stackIdx) noexcept {
  do {
    TRI_ASSERT(stackIdx != kInvalidIndex);
    auto& w = _waiters[stackIdx];
    stackIdx = State{w.next.load(std::memory_order_relaxed)}.stack();
    w.unblock();
  } while (stackIdx != kInvalidIndex);
}

void EventCount::State::check(bool waiter) const noexcept {
  [[maybe_unused]] auto w = waiters();
  TRI_ASSERT(w >= signals()) << "w: " << w << ", s: " << signals();
  TRI_ASSERT(w < kMask);
  TRI_ASSERT(!waiter || w > 0);
}

void EventCount::Waiter::block() noexcept {
  epoch += kEpochInc;
  // TODO - mem order
  auto s = status.exchange(Waiter::kWaiting, std::memory_order_acquire);
  if (s == Waiter::kNotSignaled) {
    // TODO - mem order
    status.wait(Waiter::kWaiting, std::memory_order_acquire);
  } else {
    TRI_ASSERT(s == Waiter::kSignaled);
  }
}

void EventCount::Waiter::unblock() noexcept {
  next.store(kEmptyStack, std::memory_order_relaxed);
  // TODO - mem order
  auto s = status.exchange(Waiter::kSignaled, std::memory_order_release);
  // avoid notifying if it was not waiting
  if (s == Waiter::kWaiting) {
    status.notify_one();
  }
}

}  // namespace arangodb
