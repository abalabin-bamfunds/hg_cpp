#pragma once

#include <chrono>
#include <functional>

namespace bg {

// An empty callback disables periodic invocation. Enabled callbacks run between outer cycles, with a positive
// steady-clock interval. Exceptions propagate after any started graph is stopped.
struct PeriodicCallback {
    std::function<void()> callback{};
    std::chrono::steady_clock::duration interval{std::chrono::milliseconds{50}};
};

} // namespace bg
