#pragma once

#include <bg/evaluation_interruption.hpp>

#include <chrono>
#include <stdexcept>

namespace bg::details {

class EvaluationInterruptionChecker {
  public:
    explicit EvaluationInterruptionChecker(const EvaluationInterruption &interruption)
        : interruption_{interruption} {
        if (interruption_.check && interruption_.interval <= std::chrono::steady_clock::duration::zero()) {
            throw std::invalid_argument{"evaluation interruption interval must be positive"};
        }
    }

    bool enabled() const noexcept { return static_cast<bool>(interruption_.check); }

    std::chrono::steady_clock::time_point next_check() const noexcept { return next_check_; }

    void check_now() {
        if (!enabled()) {
            return;
        }
        interruption_.check();
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::steady_clock::time_point::max() - now;
        next_check_ = interruption_.interval >= remaining
                          ? std::chrono::steady_clock::time_point::max()
                          : now + interruption_.interval;
    }

    void check_if_due() {
        if (!enabled() || std::chrono::steady_clock::now() < next_check_) {
            return;
        }
        check_now();
    }

  private:
    const EvaluationInterruption &interruption_;
    std::chrono::steady_clock::time_point next_check_{};
};

} // namespace bg::details
