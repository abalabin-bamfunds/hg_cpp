#pragma once

#include <atomic>
#include <chrono>
#include <semaphore>

namespace bg::details {

// Coalesces concurrent notifications for one engine thread. After every wake, the caller rechecks queue readiness
// and stop state. wait_until() can fail spuriously. A successful wait resets the latch with an acquire exchange so it
// imports publications from notifications coalesced after the semaphore acquire.
class EngineWakeup {
  public:
    EngineWakeup() noexcept = default;
    EngineWakeup(const EngineWakeup &) = delete;
    EngineWakeup &operator=(const EngineWakeup &) = delete;

    void notify() noexcept {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) {
            semaphore_.release();
        }
    }

    void wait() noexcept {
        semaphore_.acquire();
        pending_.exchange(false, std::memory_order_acq_rel);
    }

    template <typename Clock, typename Duration>
    bool wait_until(std::chrono::time_point<Clock, Duration> deadline) noexcept {
        using NativeTimePoint = std::chrono::time_point<Clock, typename Clock::duration>;
        // try_acquire_until requires the clock-native duration on libstdc++; saturation also handles deadlines outside
        // that duration's range without overflowing during conversion.
        const auto requested_epoch = std::chrono::duration<long double>{deadline.time_since_epoch()};
        const auto maximum_epoch = std::chrono::duration<long double>{Clock::duration::max()};
        const auto minimum_epoch = std::chrono::duration<long double>{Clock::duration::min()};
        auto native_deadline = NativeTimePoint{};
        if (requested_epoch >= maximum_epoch) {
            native_deadline = NativeTimePoint::max();
        } else if (requested_epoch <= minimum_epoch) {
            native_deadline = NativeTimePoint::min();
        } else {
            native_deadline = NativeTimePoint{
                std::chrono::duration_cast<typename Clock::duration>(deadline.time_since_epoch())};
        }

        const auto acquired = native_deadline <= Clock::now() ? semaphore_.try_acquire()
                                                              : semaphore_.try_acquire_until(native_deadline);
        if (!acquired) {
            return false;
        }
        pending_.exchange(false, std::memory_order_acq_rel);
        return true;
    }

  private:
    std::binary_semaphore semaphore_{0};
    std::atomic<bool> pending_{};
};

} // namespace bg::details
