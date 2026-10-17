#pragma once

#include <chrono>
#include <functional>

namespace bg {

// An empty check disables interruption polling. Enabled checks run between outer cycles, with a positive steady-clock
// interval. Exceptions propagate after any started graph is stopped.
struct EvaluationInterruption {
    std::function<void()> check{};
    std::chrono::steady_clock::duration interval{std::chrono::milliseconds{50}};
};

} // namespace bg
