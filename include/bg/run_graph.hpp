#pragma once

#include <bg/graph_run_input.hpp>
#include <bg/graph_run_options.hpp>
#include <bg/graph_run_result.hpp>
#include <bg/testing/test_graph.hpp>

#include <concepts>
#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

template <typename T> struct graph_run_ticks : std::false_type {};

template <std::ranges::view Range> struct graph_run_ticks<GraphRunTicks<Range>> : std::true_type {};

template <typename T> inline constexpr bool graph_run_ticks_v = graph_run_ticks<std::remove_cvref_t<T>>::value;

template <typename Argument> decltype(auto) unwrap_graph_run_ticks(Argument &&argument) {
    if constexpr (graph_run_ticks_v<Argument>) {
        return std::forward<Argument>(argument).values();
    } else {
        return std::forward<Argument>(argument);
    }
}

template <typename Tested, typename... Inputs>
auto run_graph_inputs(const Tested &tested, const GraphRunOptions &options, Inputs &&...inputs) {
    return testing::test_graph(tested, options, unwrap_graph_run_ticks(std::forward<Inputs>(inputs))...);
}

template <typename Function> auto run_graph_callback(Function &&function, const GraphRunOptions &options) {
    using FunctionInfo = testing::details::test_function<Function>;
    using OutputScalar = typename FunctionInfo::output_scalar;

    auto captured = testing::details::CapturedValues{
        options.start_time.value_or(MIN_DT + EvaluationTime::duration{1})};
    captured.set_cycle_count(1);

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        if constexpr (std::same_as<typename FunctionInfo::result, void>) {
            std::invoke(std::forward<Function>(function));
        } else {
            const auto output = std::invoke(std::forward<Function>(function));
            if (options.capture_output) {
                testing::details::capture_graph_output(output, captured);
            }
        }
    }

    return GraphRunResult<OutputScalar>{testing::details::finish_graph_run(wiring, captured, options)};
}

} // namespace details

// Wire an explicitly typed input while a run_graph callback has an active WiringContext.
inline WiringPoint graph_run_input(const GraphRunLiteralInput &input) {
    if (input.runtime_type == nullptr) {
        throw std::invalid_argument{"graph_run_input requires an explicit time-series type"};
    }
    return testing::details::wire_literal_source(input, *input.runtime_type);
}

inline WiringPoint graph_run_input(const GraphRunInput &input) {
    return testing::details::wire_runtime_source(input);
}

template <std::invocable Function>
    requires(!std::derived_from<std::remove_cvref_t<Function>, WiringTemplate>)
[[nodiscard]] auto run_graph(Function &&function, const GraphRunOptions &options = {}) {
    return details::run_graph_callback(std::forward<Function>(function), options);
}

template <typename Tested, typename... Inputs>
    requires testing::details::test_graph_target<Tested> && (sizeof...(Inputs) > 0) &&
             ((!std::same_as<std::remove_cvref_t<Inputs>, GraphRunOptions>) && ...)
[[nodiscard]] auto run_graph(const Tested &tested, Inputs &&...inputs) {
    return details::run_graph_inputs(tested, GraphRunOptions{}, std::forward<Inputs>(inputs)...);
}

template <typename Tested, typename... Inputs>
    requires testing::details::test_graph_target<Tested> && (sizeof...(Inputs) > 0) &&
             ((!std::same_as<std::remove_cvref_t<Inputs>, GraphRunOptions>) && ...)
[[nodiscard]] auto run_graph(const Tested &tested, const GraphRunOptions &options, Inputs &&...inputs) {
    return details::run_graph_inputs(tested, options, std::forward<Inputs>(inputs)...);
}

template <typename Tested>
    requires std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>
[[nodiscard]] auto run_graph(const Tested &tested) {
    return testing::test_graph(tested);
}

template <typename Tested>
    requires std::derived_from<std::remove_cvref_t<Tested>, WiringTemplate>
[[nodiscard]] auto run_graph(const Tested &tested, const GraphRunOptions &options) {
    return testing::test_graph(tested, options);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, GraphRunLiteralInput input) {
    return testing::test_graph(tested, input);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, const GraphRunOptions &options, GraphRunLiteralInput input) {
    return testing::test_graph(tested, options, input);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, GraphRunLiteralInput first, GraphRunLiteralInput second) {
    return testing::test_graph(tested, first, second);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, const GraphRunOptions &options, GraphRunLiteralInput first,
                             GraphRunLiteralInput second) {
    return testing::test_graph(tested, options, first, second);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, GraphRunLiteralInput first, GraphRunLiteralInput second,
                             GraphRunLiteralInput third) {
    return testing::test_graph(tested, first, second, third);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, const GraphRunOptions &options, GraphRunLiteralInput first,
                             GraphRunLiteralInput second, GraphRunLiteralInput third) {
    return testing::test_graph(tested, options, first, second, third);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, GraphRunLiteralInput first, GraphRunLiteralInput second,
                             GraphRunLiteralInput third, GraphRunLiteralInput fourth) {
    return testing::test_graph(tested, first, second, third, fourth);
}

template <typename Tested>
[[nodiscard]] auto run_graph(const Tested &tested, const GraphRunOptions &options, GraphRunLiteralInput first,
                             GraphRunLiteralInput second, GraphRunLiteralInput third,
                             GraphRunLiteralInput fourth) {
    return testing::test_graph(tested, options, first, second, third, fourth);
}

} // namespace bg
