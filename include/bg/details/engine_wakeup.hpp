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
        if (!semaphore_.try_acquire_until(deadline)) {
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
