#pragma once

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>

#include <concepts>
#include <functional>
#include <utility>

namespace bg::testing {

template <typename Function>
    requires std::invocable<Function> && std::same_as<std::invoke_result_t<Function>, void>
void test_wiring(Function &&function) {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    std::invoke(std::forward<Function>(function));
}

} // namespace bg::testing
