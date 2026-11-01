#pragma once

#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/wiring_context.hpp>

#include <concepts>
#include <functional>
#include <utility>

namespace hg::testing {

template <typename Function>
    requires std::invocable<Function> && std::same_as<std::invoke_result_t<Function>, void>
void test_wiring(Function &&function) {
    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    std::invoke(std::forward<Function>(function));
}

} // namespace hg::testing
