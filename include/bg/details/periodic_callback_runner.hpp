#pragma once

#include <bg/periodic_callback.hpp>

#include <chrono>
#include <stdexcept>

namespace bg::details {

class PeriodicCallbackRunner {
  public:
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
    }

    void run_if_due() {
        if (!enabled() || std::chrono::steady_clock::now() < next_run_) {
            return;
        }
        run_now();
    }

  private:
    const PeriodicCallback &periodic_callback_;
    std::chrono::steady_clock::time_point next_run_{};
};

} // namespace bg::details
