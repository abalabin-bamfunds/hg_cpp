#pragma once

#include <bg/periodic_callback.hpp>

#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace bg::details {

class PeriodicCallbackRunner {
  public:
    static constexpr auto cycles_per_poll = std::size_t{8};

    explicit PeriodicCallbackRunner(const PeriodicCallback &periodic_callback)
        : periodic_callback_{periodic_callback} {
        if (periodic_callback_.callback && periodic_callback_.interval <= std::chrono::steady_clock::duration::zero()) {
            throw std::invalid_argument{"periodic callback interval must be positive"};
        }
    }

    bool enabled() const noexcept { return static_cast<bool>(periodic_callback_.callback); }

    std::chrono::steady_clock::time_point next_run() const noexcept { return next_run_; }

    void run_now() {
        if (!enabled()) {
            return;
        }
        periodic_callback_.callback();
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::steady_clock::time_point::max() - now;
        next_run_ = periodic_callback_.interval >= remaining
                        ? std::chrono::steady_clock::time_point::max()
                        : now + periodic_callback_.interval;
        cycles_until_poll_ = cycles_per_poll;
    }

    bool run_if_due() {
        cycles_until_poll_ = cycles_per_poll;
        if (!enabled() || std::chrono::steady_clock::now() < next_run_) {
            return false;
        }
        run_now();
        return true;
    }

    void poll_after_cycle() {
        if (!enabled()) {
            return;
        }
        if (--cycles_until_poll_ == 0) {
            run_if_due();
        }
    }

  private:
    const PeriodicCallback &periodic_callback_;
    std::chrono::steady_clock::time_point next_run_{};
    std::size_t cycles_until_poll_{cycles_per_poll};
};

} // namespace bg::details
