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
#include <chrono>
#include <mutex>
#include <shared_mutex>

#include "Metrics/Histogram.h"
#include "Metrics/Gauge.h"
#include "Metrics/GaugeCounterGuard.h"

namespace arangodb {

template<typename Mutex>
struct InstrumentedMutexTraits {
  template<typename F>
  auto lock_exclusive(Mutex& m, F&& fn) requires requires {
    m.lock();
  }
  { return fn(std::unique_lock(m)); }

  template<typename F>
  auto try_lock_exclusive(Mutex& m, F&& fn) requires requires {
    { m.try_lock() } -> std::convertible_to<bool>;
  }
  { return fn(std::unique_lock(m, std::try_to_lock)); }

  template<typename F>
  auto lock_shared(Mutex& m, F&& fn) requires requires {
    m.lock_shared();
  }
  { return fn(std::shared_lock(m)); }

  template<typename F>
  auto try_lock_shared(Mutex& m, F&& fn) requires requires {
    m.try_lock_shared();
  }
  { return fn(std::shared_lock(m, std::try_to_lock)); }

  template<typename F, typename Duration>
  auto try_lock_exclusive_for(Mutex& m, Duration d, F&& fn) requires requires {
    m.try_lock_for(d);
  }
  {
    if (m.try_lock_for(d)) {
      return fn(std::unique_lock(m, std::adopt_lock));
    }
    return fn(std::unique_lock(m, std::defer_lock));
  }

  template<typename F, typename Duration>
  auto try_lock_shared_for(Mutex& m, Duration d, F&& fn) requires requires {
    m.try_lock_for(d);
  }
  {
    if (m.try_lock_shared_for(d)) {
      return fn(std::shared_lock(m, std::adopt_lock));
    }
    return fn(std::shared_lock(m, std::defer_lock));
  }
};

template<typename Mutex, typename LockGuard>
struct InstrumentedMutexLockGuardTraits {
  void unlock_exclusive(LockGuard&& guard) { guard.unlock(); }

  void unlock_shared(LockGuard&& guard) { guard.unlock(); }

  bool owns_lock(LockGuard& guard) { return guard.owns_lock(); }
};

template<typename Mutex>
struct InstrumentedMutex {
  using Traits = InstrumentedMutexTraits<Mutex>;

  using Clock = std::chrono::high_resolution_clock;

  struct Metrics {
    metrics::Gauge<uint64_t>* pendingExclusive;
    metrics::Gauge<uint64_t>* pendingShared;
    metrics::Gauge<uint64_t>* lockExclusive;
    metrics::Gauge<uint64_t>* lockShared;

    // TODO add histograms, maybe get rid of the scale
  };

  template<typename... Args>
  explicit InstrumentedMutex(Metrics const& metrics, Args&&... args)
      : _metrics(metrics), _mutex(std::forward<Args>(args)...) {}

  struct UnlockShared {
    template<typename LockGuard>
    void operator()(LockGuard guard) {
      InstrumentedMutexLockGuardTraits<Mutex, LockGuard>{}.unlock_shared(
          std::move(guard));
    }
  };
  struct UnlockExclusive {
    template<typename LockGuard>
    void operator()(LockGuard guard) {
      InstrumentedMutexLockGuardTraits<Mutex, LockGuard>{}.unlock_exclusive(
          std::move(guard));
    }
  };

  template<typename LockVariant, typename UnlockStrategy>
  struct LockGuard {
    using Traits = InstrumentedMutexLockGuardTraits<Mutex, LockVariant>;

    LockGuard() = default;
    explicit LockGuard(InstrumentedMutex* imutex,
                       metrics::Gauge<uint64_t>* gauge, LockVariant&& guard)
        : _imutex(imutex),
          _counterGuard(gauge, 1),
          _lockStart(Clock::now()),
          _guard(std::move(guard)) {}

    LockGuard(LockGuard const&) = delete;
    LockGuard& operator=(LockGuard const&) = delete;

    LockGuard(LockGuard&&) noexcept = default;
    LockGuard& operator=(LockGuard&&) noexcept = default;

    ~LockGuard() {
      if (_imutex) {
        unlock();
      }
    }

    void unlock() {
      if (_imutex) {
        UnlockStrategy{}(std::move(_guard));
        // TODO record metrics
        _counterGuard.reset();
        _imutex.reset();
      }
    }

    operator bool() const noexcept { return owns_lock(); }
    bool owns_lock() const noexcept { return _imutex != nullptr; }

   private:
    struct noop {
      void operator()(auto*) {}
    };

    std::unique_ptr<InstrumentedMutex, noop> _imutex = nullptr;
    metrics::GaugeCounterGuard<uint64_t> _counterGuard;
    Clock::time_point _lockStart;
    LockVariant _guard;
  };

  auto lock_exclusive() {
    metrics::GaugeCounterGuard<uint64_t> pendingCounter(
        _metrics.pendingExclusive, 1);
    return Traits{}.lock_exclusive(
        _mutex, [this, pendingCounter = std::move(pendingCounter)]<typename L>(
                    L&& guard) mutable {
          pendingCounter.reset();
          return LockGuard<L, UnlockShared>{this, _metrics.lockExclusive,
                                            std::forward<L>(guard)};
        });
  }

  auto lock_shared() requires
      requires(Mutex m, InstrumentedMutexTraits<Mutex> t) {
    t.lock_shared(m, [](auto&&) {});
  }
  {
    metrics::GaugeCounterGuard<uint64_t> pendingCounter(
        _metrics.pendingExclusive, 1);
    return Traits{}.lock_shared(
        _mutex, [this, pendingCounter = std::move(pendingCounter)]<typename L>(
                    L&& guard) mutable {
          pendingCounter.reset();
          return LockGuard<L, UnlockShared>{this, _metrics.lockShared,
                                            std::forward<L>(guard)};
        });
  }

  template<typename Duration>
  auto try_lock_exclusive_for(Duration d) requires
      requires(Mutex m, InstrumentedMutexTraits<Mutex> t) {
    t.try_lock_exclusive_for(m, d, [](auto&&) {});
  }
  {
    metrics::GaugeCounterGuard<uint64_t> pendingCounter(
        _metrics.pendingExclusive, 1);
    return Traits{}.try_lock_exclusive_for(
        _mutex, d,
        [this, pendingCounter =
                   std::move(pendingCounter)]<typename L>(L&& guard) mutable {
          pendingCounter.reset();
          using LockTraits = InstrumentedMutexLockGuardTraits<Mutex, L>;
          if (LockTraits{}.owns_lock(guard)) {
            return LockGuard<L, UnlockExclusive>{this, _metrics.lockExclusive,
                                                 std::forward<L>(guard)};
          }

          return LockGuard<L, UnlockExclusive>{};
        });
  }

  auto try_lock_exclusive() requires
      requires(Mutex m, InstrumentedMutexTraits<Mutex> t) {
    t.try_lock_exclusive(m, [](auto&&) {});
  }
  {
    return Traits{}.try_lock_exclusive(
        _mutex, [this]<typename L>(L&& guard) mutable {
          using LockTraits = InstrumentedMutexLockGuardTraits<Mutex, L>;
          if (LockTraits{}.owns_lock(guard)) {
            return LockGuard<L, UnlockExclusive>{this, _metrics.lockExclusive,
                                                 std::forward<L>(guard)};
          }

          return LockGuard<L, UnlockExclusive>{};
        });
  }

  auto try_lock_shared() requires
      requires(Mutex m, InstrumentedMutexTraits<Mutex> t) {
    t.try_lock_shared(m, [](auto&&) {});
  }
  {
    return Traits{}.try_lock_shared(
        _mutex, [this]<typename L>(L&& guard) mutable {
          using LockTraits = InstrumentedMutexLockGuardTraits<Mutex, L>;
          if (LockTraits{}.owns_lock(guard)) {
            return LockGuard<L, UnlockShared>{this, _metrics.lockShared,
                                              std::forward<L>(guard)};
          }

          return LockGuard<L, UnlockShared>{};
        });
  }

  template<typename Duration>
  auto try_lock_shared_for(Duration d) requires
      requires(Mutex m, InstrumentedMutexTraits<Mutex> t) {
    t.try_lock_shared_for(m, d, [](auto&&) {});
  }
  {
    metrics::GaugeCounterGuard<uint64_t> pendingCounter(
        _metrics.pendingExclusive, 1);
    return Traits{}.try_lock_shared_for(
        _mutex, d,
        [this, pendingCounter =
                   std::move(pendingCounter)]<typename L>(L&& guard) mutable {
          pendingCounter.reset();
          using LockTraits = InstrumentedMutexLockGuardTraits<Mutex, L>;
          if (LockTraits{}.owns_lock(guard)) {
            return LockGuard<L, UnlockShared>{this, _metrics.lockShared,
                                              std::forward<L>(guard)};
          }

          return LockGuard<L, UnlockShared>{};
        });
  }

  Metrics _metrics;
  Mutex _mutex;
};

}  // namespace arangodb
