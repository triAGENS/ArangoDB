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
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace arangodb {

// This EventCount implementation is heavily inspired by the Eigen EventCount
// implementation by Dmitry Vyukov
// https://gitlab.com/libeigen/eigen/-/blob/5e4f3475b5cb77414bca1f3dde7d6fd9cb4d2011/Eigen/src/ThreadPool/EventCount.h

struct EventCount {
  struct PendingWait {
    ~PendingWait();

    PendingWait() = delete;
    PendingWait(PendingWait&&) = delete;
    PendingWait(PendingWait const&) = delete;
    PendingWait& operator=(PendingWait&&) = delete;
    PendingWait& operator=(PendingWait const&) = delete;

    void commit() noexcept;
    void cancel() noexcept;

   private:
    friend struct EventCount;
    PendingWait(EventCount& ec, std::size_t waiterIndex) noexcept;

    EventCount& _ec;
    std::size_t _waiterIndex;
    static constexpr std::size_t kInvalidIndex =
        std::numeric_limits<std::size_t>::max();
  };

  EventCount(std::size_t numWaiters) noexcept;
  ~EventCount();

  EventCount(EventCount&&) = delete;
  EventCount(EventCount const&) = delete;
  EventCount& operator=(EventCount&&) = delete;
  EventCount& operator=(EventCount const&) = delete;

  /// @brief after calling prepareWait, the thread must re-check the wait
  /// predicate and then call either cancel or commit on the returned
  /// PendingWait object.
  auto prepareWait(std::size_t waiterIndex) noexcept -> PendingWait;

  // wake or signal one waiter.
  // returns true if successful, false if there are no waiters
  auto notifyOne(std::size_t& notified) noexcept -> bool {
    return notify(false, notified);
  }
  void notifyOne() noexcept {
    std::size_t notified;
    notify(false, notified);
  }
  void notifyAll() noexcept {
    std::size_t notified;
    notify(true, notified);
  }

#ifdef ARANGODB_USE_GOOGLE_TESTS
  auto getNumWaiters() const noexcept -> std::size_t;
  auto getNumSignals() const noexcept -> std::size_t;
  auto getWaiterStack() const noexcept -> std::vector<std::size_t>;
#endif

 private:
  /// @brief commits wait operation after prepareWait
  void commitWait(std::size_t waiterIndex) noexcept;

  /// @brief cancels effects of the previous prepareWait call
  void cancelWait(std::size_t waiterIndex) noexcept;

  auto notify(bool notifyAll, std::size_t& notified) noexcept -> bool;

  void unblockAll(std::size_t stackIdx) noexcept;

  // State layout:
  //   | epoch (20bits) | stack (14bits) | waiters (14bits) | signals (14bits) |
  //
  // - signals is the number of pending signals
  // - waiters is the number of waiters in prepareWait state
  // - stack is a "linked list" (via indexes) of waiters in committed wait state
  // - epoch is an ABA counter for the stack (stored in Waiter node's epoch
  //   field and incremented on push)
  static constexpr std::uint64_t kBits = 14;
  static constexpr std::uint64_t kMask = (1ull << kBits) - 1;
  static constexpr std::uint64_t kMaxWaiters = kMask;

  static constexpr std::uint64_t kSignalInc = 1ull;
  static constexpr std::uint64_t kSignalMask = kMask;

  static constexpr std::uint64_t kWaiterShift = kBits;
  static constexpr std::uint64_t kWaiterInc = 1ull << kWaiterShift;
  static constexpr std::uint64_t kWaiterMask = kMask << kWaiterShift;

  static constexpr std::uint64_t kStackShift = 2 * kBits;
  static constexpr std::uint64_t kEmptyStack = kMask << kStackShift;
  static constexpr std::uint64_t kInvalidIndex = kMask;

  static constexpr std::uint64_t kEpochShift = 3 * kBits;
  static constexpr std::uint64_t kEpochInc = 1ull << kEpochShift;
  static constexpr std::uint64_t kEpochBits = 64 - kEpochShift;
  static constexpr std::uint64_t kEpochMask = ((1ull << kEpochBits) - 1)
                                              << kEpochShift;
  static_assert(kEpochBits >= 20, "not enough bits to prevent ABA problem");

  static constexpr std::uint64_t kEpochStackMask = kEpochMask | kEmptyStack;

  static_assert((kEpochStackMask & kWaiterMask) == 0);
  static_assert((kEpochStackMask & kSignalMask) == 0);
  static_assert(~kEpochStackMask == (kWaiterMask | kSignalMask));

  struct State {
    explicit State(std::uint64_t value) : value(value) { check(); }

    bool operator==(const State& other) const noexcept = default;

    std::uint64_t stack() const noexcept {
      return (value >> kStackShift) & kMask;
    }
    std::uint64_t waiters() const noexcept {
      return (value >> kWaiterShift) & kMask;
    }
    std::uint64_t signals() const noexcept { return value & kMask; }

    State incWaiter() const noexcept { return State(value + kWaiterInc); }
    State decWaiter() const noexcept { return State(value - kWaiterInc); }

    State incSignal() const noexcept { return State(value + kSignalInc); }
    State decSignal() const noexcept { return State(value - kSignalInc); }

    void check(bool waiter = false) const noexcept;

    std::uint64_t value;
  };

  struct AtomicState {
    AtomicState() : _state(kEmptyStack) {}

    auto load(std::memory_order order =
                  std::memory_order_seq_cst) const noexcept -> State {
      return State{_state.load(order)};
    }

    auto compare_exchange_weak(State& expected, State desired,
                               std::memory_order m1,
                               std::memory_order m2) noexcept -> bool {
      auto res =
          _state.compare_exchange_weak(expected.value, desired.value, m1, m2);
      if (!res) {
        expected.check();
      }
      return res;
    }

    auto incWaiter(std::memory_order order = std::memory_order_seq_cst) noexcept
        -> State {
      return State{_state.fetch_add(kWaiterInc, order) + kWaiterInc};
    }

   private:
    std::atomic<std::uint64_t> _state;
  };

  // we align Waiters to 128 byte boundary to prevent false sharing with other
  // Waiter objects in the same vector.
  struct alignas(128) Waiter {
    enum Status : std::int32_t {
      kActive,
      kPreparedWait,
      kNotSignaled,
      kWaiting,
      kSignaled,
    };

    void block() noexcept;
    void unblock() noexcept;

    std::atomic<std::uint64_t> next;
    std::atomic<Status> status = kActive;
    std::uint64_t epoch = 0;
  };

  AtomicState _state;
  std::vector<Waiter> _waiters;
};

}  // namespace arangodb
